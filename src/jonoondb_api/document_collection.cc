#include <string>
#include <boost/filesystem.hpp>
#include <unordered_map>
#include <string>
#include "sqlite3.h"
#include "document_collection.h"
#include "string_utils.h"
#include "exception_utils.h"
#include "sqlite_utils.h"
#include "document.h"
#include "document_factory.h"
#include "index_manager.h"
#include "index_info_impl.h"
#include "document_schema.h"
#include "document_schema_factory.h"
#include "enums.h"
#include "jonoondb_exceptions.h"
#include "index_stat.h"
#include "mama_jennies_bitmap.h"
#include "blob_manager.h"
#include "constraint.h"
#include "buffer_impl.h"
#include "file_info.h"
#include "filename_manager.h"
#include "jonoondb_api/write_options_impl.h"

using namespace jonoondb_api;

DocumentCollection::DocumentCollection(const std::string& databaseMetadataFilePath,
                                       const std::string& name,
                                       SchemaType schemaType,
                                       const std::string& schema,
                                       const std::vector<IndexInfoImpl*>& indexes,
                                       std::unique_ptr<BlobManager> blobManager,
                                       const std::vector<FileInfo>& dataFilesToLoad)
    :
    m_blobManager(move(blobManager)),
    m_dbConnection(nullptr, SQLiteUtils::CloseSQLiteConnection) {
  // Validate function arguments
  if (databaseMetadataFilePath.size() == 0) {
    throw InvalidArgumentException("Argument databaseMetadataFilePath is empty.",
                                   __FILE__,
                                   __func__,
                                   __LINE__);
  }

  if (name.size() == 0) {
    throw InvalidArgumentException("Argument name is empty.",
                                   __FILE__,
                                   __func__,
                                   __LINE__);
  }
  m_name = name;

  // databaseMetadataFile should exist and all the tables should exist in it
  if (!boost::filesystem::exists(databaseMetadataFilePath)) {
    std::ostringstream ss;
    ss << "Database file " << databaseMetadataFilePath << " does not exist.";
    throw MissingDatabaseFileException(ss.str(), __FILE__, __func__, __LINE__);
  }

  sqlite3* dbConnection = nullptr;
  int sqliteCode = sqlite3_open(databaseMetadataFilePath.c_str(),
                                &dbConnection);  //, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  m_dbConnection.reset(dbConnection);

  if (sqliteCode != SQLITE_OK) {
    throw SQLException(sqlite3_errstr(sqliteCode),
                       __FILE__,
                       __func__,
                       __LINE__);
  }

  m_documentSchema.reset(DocumentSchemaFactory::CreateDocumentSchema(schema,
                                                                     schemaType));

  unordered_map<string, FieldType> columnTypes;
  PopulateColumnTypes(indexes, *m_documentSchema.get(), columnTypes);
  m_indexManager.reset(new IndexManager(indexes, columnTypes));

  // Load the data files
  for (auto& file : dataFilesToLoad) {
    BlobIterator iter(file);
    const std::size_t desiredBatchSize = 10000;
    std::vector<BufferImpl> blobs(desiredBatchSize);
    std::vector<BlobMetadata> blobMetadataVec(desiredBatchSize);
    std::size_t actualBatchSize = 0;

    while ((actualBatchSize = iter.GetNextBatch(blobs, blobMetadataVec)) > 0) {
      std::vector<std::unique_ptr<Document>> docs;
      for (size_t i = 0; i < actualBatchSize; i++) {
        // Todo optimize the creation of doc creation
        // we should reuse documents
        docs.push_back(DocumentFactory::CreateDocument(*m_documentSchema,
                                                       blobs[i]));
      }

      auto
          startID = m_indexManager->IndexDocuments(m_documentIDGenerator, docs);
      assert(startID == m_documentIDMap.size());
      m_documentIDMap.insert(m_documentIDMap.end(), blobMetadataVec.begin(),
                             blobMetadataVec.begin() + actualBatchSize);
    }
  }
}

void DocumentCollection::Insert(const BufferImpl& documentData,
                                const WriteOptionsImpl& wo) {
  std::vector<const BufferImpl*> vec = {&documentData};
  gsl::span<const BufferImpl*> span = vec;
  MultiInsert(span, wo);
}

void jonoondb_api::DocumentCollection::MultiInsert(
    gsl::span<const BufferImpl*>& documents, const WriteOptionsImpl& wo) {
  std::vector<std::unique_ptr<Document>> docs;

  for (size_t i = 0; i < documents.size(); i++) {
    docs.push_back(DocumentFactory::CreateDocument(*m_documentSchema,
                                                   *documents[i]));
    if (wo.verifyDocuments && !docs.back()->Verify()) {
      ostringstream ss;
      ss << "Document at index location " << i << " is not valid.";
      throw JonoonDBException(ss.str(), __FILE__, __func__, __LINE__);
    }
  }  

  std::vector<BlobMetadata> blobMetadataVec(documents.size());
  // Indexing should not fail after we have called ValidateForIndexing
  try {
    auto startID = m_indexManager->IndexDocuments(m_documentIDGenerator, docs);
    assert(startID == m_documentIDMap.size());
    m_blobManager->MultiPut(documents, blobMetadataVec, wo.compress);
  } catch (...) {
    // This is a serious error. Exception at this point will leave DB in a invalid state.
    // Only thing we can do here is to log the error and terminate the process.
    // Todo: log and terminate the process
    throw;
  }

  m_documentIDMap.insert(m_documentIDMap.end(),
                         blobMetadataVec.begin(),
                         blobMetadataVec.end());
}

const std::string& DocumentCollection::GetName() {
  return m_name;
}

const std::shared_ptr<DocumentSchema>& DocumentCollection::GetDocumentSchema() {
  return m_documentSchema;
}

bool DocumentCollection::TryGetBestIndex(const std::string& columnName,
                                         IndexConstraintOperator op,
                                         IndexStat& indexStat) {
  return m_indexManager->TryGetBestIndex(columnName, op, indexStat);
}

std::shared_ptr<MamaJenniesBitmap> DocumentCollection::Filter(const std::vector<
    Constraint>& constraints) {
  if (constraints.size() > 0) {
    return m_indexManager->Filter(constraints);
  } else {
    // Return all the ids
    auto lastID = m_documentIDMap.size();
    auto bm = std::make_shared<MamaJenniesBitmap>();
    for (std::size_t i = 0; i < lastID; i++) {
      bm->Add(i);
    }

    return bm;
  }
}

void DocumentCollection::GetDocumentAndBuffer(
  std::uint64_t docID, std::unique_ptr<Document>& document,
  BufferImpl& buffer) const {
  if (docID >= m_documentIDMap.size()) {
    ostringstream ss;
    ss << "Document with ID '" << docID << "' does exist in collection "
      << m_name << ".";
    throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
  }

  m_blobManager->Get(m_documentIDMap.at(docID), buffer);
  document = DocumentFactory::CreateDocument(*m_documentSchema, buffer);
}

bool DocumentCollection::TryGetBlobFieldFromIndexer(
    std::uint64_t docID, const std::string& columnName,
    BufferImpl& val) const {
  if (docID >= m_documentIDMap.size()) {
    ostringstream ss;
    ss << "Document with ID '" << docID << "' does exist in collection "
      << m_name << ".";
    throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
  }

  // lets see if we can get this value from any index  
  if (m_indexManager->TryGetBlobValue(docID, columnName, val)) {
    return true;
  } 

  return false;
}

bool DocumentCollection::TryGetIntegerFieldFromIndexer(
  std::uint64_t docID, const std::string& columnName,
  std::int64_t& val) const {
  if (docID >= m_documentIDMap.size()) {
    ostringstream ss;
    ss << "Document with ID '" << docID << "' does exist in collection "
      << m_name << ".";
    throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
  }

  // lets see if we can get this value from any index  
  if (m_indexManager->TryGetIntegerValue(docID, columnName, val)) {
    return true;
  }

  return false;
}

bool DocumentCollection::TryGetFloatFieldFromIndexer(
  std::uint64_t docID, const std::string& columnName,
  double& val) const {
  if (docID >= m_documentIDMap.size()) {
    ostringstream ss;
    ss << "Document with ID '" << docID << "' does exist in collection "
      << m_name << ".";
    throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
  }

  // lets see if we can get this value from any index  
  if (m_indexManager->TryGetDoubleValue(docID, columnName, val)) {
    return true;
  }

  return false;
}

bool DocumentCollection::TryGetStringFieldFromIndexer(
  std::uint64_t docID, const std::string& columnName,
  std::string& val) const {
  if (docID >= m_documentIDMap.size()) {
    ostringstream ss;
    ss << "Document with ID '" << docID << "' does exist in collection "
      << m_name << ".";
    throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
  }

  // lets see if we can get this value from any index  
  if (m_indexManager->TryGetStringValue(docID, columnName, val)) {
    return true;
  }

  return false;
}

void DocumentCollection::GetDocumentFieldsAsIntegerVector(
    const gsl::span<std::uint64_t>& docIDs, const std::string& columnName,
    const std::vector<std::string>& tokens,
    std::vector<std::int64_t>& values) const {
  if (tokens.size() == 0) {
    throw InvalidArgumentException("Argument tokens is empty.", __FILE__,
                                   "", __LINE__);
  }

  if (m_indexManager->TryGetIntegerVector(docIDs, columnName, values)) {
    // We have the values
    return;
  }

  BufferImpl buffer;
  assert(docIDs.size() == values.size());
  std::unique_ptr<Document> subDoc;
  for (int i = 0; i < docIDs.size(); i++) {
    if (docIDs[i] >= m_documentIDMap.size()) {
      ostringstream ss;
      ss << "Document with ID '" << docIDs[i]
          << "' does exist in collection " << m_name << ".";
      throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
    }

    m_blobManager->Get(m_documentIDMap.at(docIDs[i]), buffer);

    auto document = DocumentFactory::CreateDocument(*m_documentSchema, buffer);
    if (!subDoc) {
      subDoc = document->AllocateSubDocument();
    }
    values[i] = DocumentUtils::GetIntegerValue(*document.get(), subDoc, tokens);
  }
}

void DocumentCollection::GetDocumentFieldsAsDoubleVector(
    const gsl::span<std::uint64_t>& docIDs,
    const std::string& columnName,
    const std::vector<std::string>& tokens,
    std::vector<double>& values) const {
  if (tokens.size() == 0) {
    throw InvalidArgumentException("Argument tokens is empty.", __FILE__,
                                   "", __LINE__);
  }

  if (m_indexManager->TryGetDoubleVector(docIDs, columnName, values)) {
    // We have the values
    return;
  }

  BufferImpl buffer;
  assert(docIDs.size() == values.size());
  std::unique_ptr<Document> subDoc;
  for (int i = 0; i < docIDs.size(); i++) {
    if (docIDs[i] >= m_documentIDMap.size()) {
      ostringstream ss;
      ss << "Document with ID '" << docIDs[i]
          << "' does exist in collection " << m_name << ".";
      throw MissingDocumentException(ss.str(), __FILE__, __func__, __LINE__);
    }

    m_blobManager->Get(m_documentIDMap.at(docIDs[i]), buffer);

    auto document = DocumentFactory::CreateDocument(*m_documentSchema, buffer);
    if (!subDoc) {
      subDoc = document->AllocateSubDocument();
    }
    values[i] = DocumentUtils::GetFloatValue(*document.get(), subDoc, tokens);
  }
}

void DocumentCollection::UnmapLRUDataFiles() {
  m_blobManager->UnmapLRUDataFiles();
}

void DocumentCollection::PopulateColumnTypes(
    const std::vector<IndexInfoImpl*>& indexes,
    const DocumentSchema& documentSchema,
    std::unordered_map<string, FieldType>& columnTypes) {
  for (std::size_t i = 0; i < indexes.size(); i++) {
    columnTypes.insert(
        pair<string, FieldType>(indexes[i]->GetColumnName(),
                                documentSchema.GetFieldType(indexes[i]->GetColumnName())));
  }
}
