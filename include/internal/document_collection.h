#pragma once

#include <memory>
#include <unordered_map>
#include <string>
#include <cstdint>
#include "index_manager.h"
#include "document_id_generator.h"
#include "blob_metadata.h"

// Forward declaration
struct sqlite3;

namespace jonoondb_api {
// Forward Declaration
class Status;
class IndexInfo;
class Buffer;
class DocumentSchema;
class IndexStat;
enum class FieldType
: std::int32_t;
enum class SchemaType
: std::int32_t;

class DocumentCollection {
 public:
  ~DocumentCollection();
  static Status Construct(const char* databaseMetadataFilePath,
                          const char* name, SchemaType schemaType,
                          const char* schema, const IndexInfo indexes[],
                          size_t indexesLength,
                          DocumentCollection*& documentCollection);

  Status Insert(const Buffer& documentData);
  const std::string& GetName();
  const std::shared_ptr<DocumentSchema>& GetDocumentSchema();
  bool TryGetBestIndex(const std::string& columnName, IndexConstraintOperator op,
                       IndexStat& indexStat);

 private:
  explicit DocumentCollection(const char* name, sqlite3* dbConnection,
                              std::unique_ptr<IndexManager> indexManager,
                              std::shared_ptr<DocumentSchema> documentSchema);
  static Status PopulateColumnTypes(
      const IndexInfo indexes[], size_t indexesLength,
      const DocumentSchema& documentSchema,
      std::unordered_map<std::string, FieldType>& columnTypes);
  sqlite3* m_dbConnection;
  std::unique_ptr<IndexManager> m_indexManager;
  std::shared_ptr<DocumentSchema> m_documentSchema;
  DocumentIDGenerator m_documentIDGenerator;
  std::unordered_map<std::uint64_t, BlobMetadata> m_documentIDMap;
  std::string m_name;
};
}  // namespace jonoondb_api

