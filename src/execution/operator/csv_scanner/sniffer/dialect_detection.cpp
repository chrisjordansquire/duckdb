#include "duckdb/execution/operator/persistent/csv_scanner/csv_sniffer.hpp"

namespace duckdb {

void CSVSniffer::ResetStats() {
	// Reset stats
	rows_read = 0;
	best_consistent_rows = 0;
	prev_padding_count = 0;
	best_num_cols = 0;
}

void CSVSniffer::GenerateCandidateDetectionSearchSpace() {
	if (options.has_delimiter) {
		// user provided a delimiter: use that delimiter
		delim_candidates = {options.delimiter};
	} else {
		// no delimiter provided: try standard/common delimiters
		delim_candidates = {',', '|', ';', '\t'};
	}
	if (options.has_quote) {
		// user provided quote: use that quote rule
		quote_candidates_map = {{options.quote}, {options.quote}, {options.quote}};
	} else {
		// no quote rule provided: use standard/common quotes
		quote_candidates_map = {{'\"'}, {'\"', '\''}, {'\0'}};
	}
	if (options.has_escape) {
		// user provided escape: use that escape rule
		if (options.escape == '\0') {
			quoterule_candidates = {QuoteRule::QUOTES_RFC};
		} else {
			quoterule_candidates = {QuoteRule::QUOTES_OTHER};
		}
		escape_candidates_map[static_cast<uint8_t>(quoterule_candidates[0])] = {options.escape};
	} else {
		// no escape provided: try standard/common escapes
		quoterule_candidates = {QuoteRule::QUOTES_RFC, QuoteRule::QUOTES_OTHER, QuoteRule::NO_QUOTES};
	}
}

void CSVSniffer::GenerateStateMachineSearchSpace() {
	// Generate state machines for all option combinations
	for (auto quoterule : quoterule_candidates) {
		const auto &quote_candidates = quote_candidates_map[static_cast<uint8_t>(quoterule)];
		for (const auto &quote : quote_candidates) {
			for (const auto &delim : delim_candidates) {
				const auto &escape_candidates = escape_candidates_map[static_cast<uint8_t>(quoterule)];
				for (const auto &escape : escape_candidates) {
					auto state_options = options;
					state_options.quote = quote;
					state_options.escape = escape;
					state_options.delimiter = delim;
					D_ASSERT(buffer_manager);
					csv_state_machines.emplace_back(state_options, buffer_manager);
				}
			}
		}
	}
}

void CSVSniffer::AnalyzeDialectCandidate(CSVStateMachine &state_machine, idx_t prev_column_count) {
	vector<idx_t> sniffed_column_counts(STANDARD_VECTOR_SIZE);
	state_machine.SniffDialect(sniffed_column_counts);

	idx_t start_row = options.skip_rows;
	idx_t consistent_rows = 0;
	idx_t num_cols = sniffed_column_counts.empty() ? 0 : sniffed_column_counts[0];
	idx_t padding_count = 0;
	bool allow_padding = options.null_padding;
	if (sniffed_column_counts.size() > rows_read) {
		rows_read = sniffed_column_counts.size();
	}
	for (idx_t row = 0; row < sniffed_column_counts.size(); row++) {
		if (sniffed_column_counts[row] == num_cols) {
			consistent_rows++;
		} else if (num_cols < sniffed_column_counts[row] && !options.skip_rows_set) {
			// all rows up to this point will need padding
			padding_count = 0;
			// we use the maximum amount of num_cols that we find
			num_cols = sniffed_column_counts[row];
			start_row = row + options.skip_rows;
			consistent_rows = 1;

		} else if (num_cols >= sniffed_column_counts[row]) {
			// we are missing some columns, we can parse this as long as we add padding
			padding_count++;
		}
	}

	if (num_cols < prev_column_count) {
		// Early return if we have less columns than the previous chunk run
		return;
	}

	// some logic
	consistent_rows += padding_count;
	bool more_values = (consistent_rows > best_consistent_rows && num_cols >= best_num_cols);
	bool require_more_padding = padding_count > prev_padding_count;
	bool require_less_padding = padding_count < prev_padding_count;
	bool single_column_before = best_num_cols < 2 && num_cols > best_num_cols;
	bool rows_consistent = start_row + consistent_rows - options.skip_rows == sniffed_column_counts.size();
	bool more_than_one_row = (consistent_rows > 1);
	bool more_than_one_column = (num_cols > 1);
	bool start_good = !candidates.empty() && (start_row <= candidates.front().state->start_row);
	bool invalid_padding = !allow_padding && padding_count > 0;

	if (!requested_types.empty() && requested_types.size() != num_cols && !invalid_padding) {
		return;
	} else if (rows_consistent &&
	           (single_column_before || (more_values && !require_more_padding) ||
	            (more_than_one_column && require_less_padding)) &&
	           !invalid_padding) {
		best_consistent_rows = consistent_rows;
		best_num_cols = num_cols;
		prev_padding_count = padding_count;
		state_machine.start_row = start_row;
		candidates.clear();
		candidates.emplace_back(&state_machine, num_cols);
	} else if (more_than_one_row && more_than_one_column && start_good && rows_consistent && !require_more_padding &&
	           !invalid_padding) {
		bool same_quote_is_candidate = false;
		for (auto &candidate : candidates) {
			if (state_machine.options.quote == candidate.state->options.quote) {
				same_quote_is_candidate = true;
			}
		}
		if (!same_quote_is_candidate) {
			state_machine.start_row = start_row;
			candidates.emplace_back(&state_machine, num_cols);
		}
	}
}

void CSVSniffer::RefineCandidates() {
	auto cur_best_num_cols = best_num_cols;
	for (idx_t i = 1; i < options.sample_chunks; i++) {
		if (candidates.size() <= 1) {
			// no candidates or we only have one candidate left: stop
			return;
		}
		bool finished_file = candidates[0].state->csv_buffer_iterator.Finished();
		if (finished_file) {
			// we finished the file: stop
			return;
		}
		ResetStats();
		auto cur_candidates = std::move(candidates);
		cur_best_num_cols = std::max(best_num_cols, cur_best_num_cols);
		for (auto &cur_candidate : cur_candidates) {
			AnalyzeDialectCandidate(*cur_candidate.state, cur_best_num_cols);
		}
	}
}

// Dialect Detection consists of five steps:
// 1. Generate a search space of all possible dialects
// 2. Generate a state machine for each dialect
// 3. Analyze the first chunk of the file and find the best dialect candidates
// 4. Analyze the remaining chunks of the file and find the best dialect candidate
void CSVSniffer::DetectDialect() {
	// Step 1: Generate search space
	GenerateCandidateDetectionSearchSpace();
	// Step 2: Generate state machines
	GenerateStateMachineSearchSpace();
	// Step 3: Analyze all candidates on the first chunk
	for (auto &state_machine : csv_state_machines) {
		AnalyzeDialectCandidate(state_machine);
	}
	// Step 4: Loop over candidates and find if they can still produce good results for the remaining chunks
	RefineCandidates();
	// if no dialect candidate was found, we throw an exception
	if (candidates.empty()) {
		throw InvalidInputException(
		    "Error in file \"%s\": CSV options could not be auto-detected. Consider setting parser options manually.",
		    options.file_path);
	}
}
} // namespace duckdb
