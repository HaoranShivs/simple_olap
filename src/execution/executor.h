namespace simple_olap
{
    class ExecutorBuilder
    {
    public:
        std::unique_ptr<Operator>
        Build(const PhysicalPlan &plan);
    };
} // namespace simple_olap
