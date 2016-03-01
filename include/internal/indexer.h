#pragma once

#include <memory>

namespace jonoondb_api {
// Forward declarations
class IndexInfoImpl;
class Document;
class IndexStat;
struct Constraint;
class MamaJenniesBitmap;

class Indexer {
 public:
  virtual ~Indexer() {
  }
  // This function validates that the document is valid for insert.
  // If this function returns without exception then the insert function call on the same document must succeed.
  virtual void ValidateForInsert(const Document& document) = 0;
  virtual void Insert(std::uint64_t documentID, const Document& document) = 0;
  virtual const IndexStat& GetIndexStats() = 0;
  virtual std::shared_ptr<MamaJenniesBitmap> Filter(const Constraint& constraint) = 0;
  virtual std::shared_ptr<MamaJenniesBitmap> FilterRange(
      const Constraint& lowerConstraint,
      const Constraint& upperConstraint) = 0;

  virtual bool TryGetIntegerValue(std::uint64_t documentID, std::int64_t& val) {
    return false;
  }

  virtual bool TryGetDoubleValue(std::uint64_t documentID, double& val) {
    return false;
  }

  virtual bool TryGetStringValue(std::uint64_t documentID, std::string& val) {
    return false;
  }
};
} // namespace jonoondb_api
