namespace simple_olap
{
    struct ProjectionSpec
    {
        std::vector<uint32_t> input_slots;
    };

    class ProjectionOperator : public Operator
    {
    public:
        ProjectionOperator(
            std::unique_ptr<Operator> child,
            ProjectionSpec spec);

        void Init() override;

        bool Next(VectorBatch &output) override;

    private:
        std::unique_ptr<Operator> child_;

        ProjectionSpec spec_;

        VectorBatch input_;
    };
}