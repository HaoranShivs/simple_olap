namespace simple_olap {
std::unique_ptr<Operator> ExecutorBuilder::BuildSeqScan(const PhysicalSeqScan& plan) {
    ScanSpec spec;

    spec.table_id = plan.TableOid();

    spec.columns = plan.Columns();

    if (plan.Predicate()) {
        spec.pushed_predicate = BuildStorageCondition(*plan.Predicate());
    }

    return std::make_unique<SeqScanOperator>(std::move(spec), ctx_);
}

std::unique_ptr<Operator> ExecutorBuilder::Build(const PhysicalPlan& plan) {
    switch (plan.Type()) {
    case PhysicalPlanType::SEQ_SCAN:
        return BuildSeqScan(static_cast<const PhysicalSeqScan&>(plan));

    case PhysicalPlanType::FILTER:
        return BuildFilter(static_cast<const PhysicalFilter&>(plan));

    case PhysicalPlanType::PROJECT:
        return BuildProject(static_cast<const PhysicalProject&>(plan));

    case PhysicalPlanType::HASH_AGGREGATE:
        return BuildAggregate(static_cast<const PhysicalHashAggregate&>(plan));

    default:
        throw std::runtime_error("unsupported physical plan");
    }
}

std::unique_ptr<Operator> ExecutorBuilder::BuildFilter(const PhysicalFilter& plan) {
    auto child = Build(*plan.Child());

    auto predicate = CompilePredicate(*plan.Predicate(), plan.Child()->OutputSchema());

    return std::make_unique<FilterOperator>(std::move(child), std::move(predicate));
}
} // namespace simple_olap
