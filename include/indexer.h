#pragma once

namespace jonoondb_api {

// Forward declarations
class Status;
class IndexInfo;
class Document;
class IndexStat;

class Indexer {
 public:
  virtual ~Indexer() {
  }
  // This function validates that the document is valid for insert.
  // If this function returns OK status then the insert function call on the same document must succeed.
  virtual Status ValidateForInsert(const Document& document) = 0;
  virtual void Insert(std::uint64_t documentID, const Document& document) = 0;
  virtual const IndexStat& GetIndexStats() = 0;
};
}
