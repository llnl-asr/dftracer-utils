#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H

namespace dftracer::utils::utilities::indexer::internal {

template <typename Database>
class TransactionScope {
   public:
    explicit TransactionScope(Database& db) : db_(db) {
        db_.begin_transaction();
    }

    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;

    TransactionScope(TransactionScope&& other) noexcept
        : db_(other.db_), committed_(other.committed_) {
        other.committed_ = true;
    }

    ~TransactionScope() {
        if (!committed_) {
            db_.rollback_transaction();
        }
    }

    void commit() {
        db_.commit_transaction();
        committed_ = true;
    }

   private:
    Database& db_;
    bool committed_ = false;
};

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_TRANSACTION_SCOPE_H
