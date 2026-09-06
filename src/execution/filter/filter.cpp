namespace simple_olap
{
    bool FilterOperator::Next(VectorBatch &output)
    {
        while (child_->Next(output))
        {
            const auto &column =
                output.columns[predicate_.input_col_idx];

            std::vector<uint32_t> new_selection;

            new_selection.reserve(output.size);

            for (uint32_t i = 0;
                 i < output.size;
                 ++i)
            {
                uint32_t row =
                    output.sel_vector[i];

                if (EvaluatePredicate(
                        column,
                        row,
                        predicate_))
                {
                    new_selection.push_back(row);
                }
            }

            output.sel_vector =
                std::move(new_selection);

            output.size =
                static_cast<uint32_t>(
                    output.sel_vector.size());

            if (output.size > 0)
            {
                return true;
            }
        }

        return false;
    }
} // namespace simple_olap
