namespace simple_olap
{
    struct FilterPredicate
    {
        uint32_t input_col_idx; // 不是ColumnId
        CmpOp op;

        LiteralValue value;

        class FilterOperator : public Operator
        {
        public:
            FilterOperator(
                std::unique_ptr<Operator> child,
                FilterPredicate predicate)
                : child_(std::move(child)),
                  predicate_(std::move(predicate))
            {
            }

            void Init() override
            {
                child_->Init();
            }

            bool Next(VectorBatch &output) override;

        private:
            std::unique_ptr<Operator> child_;
            FilterPredicate predicate_;
        };
    };
} // namespace simple_olap
