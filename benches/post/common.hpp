#ifndef FIONA_BENCHES_POST_COMMON_HPP
#define FIONA_BENCHES_POST_COMMON_HPP

inline constexpr int const num_threads = 8;
inline constexpr int const num_tasks = 250'000;
inline constexpr int const total_runs = num_threads * num_tasks;

#endif // FIONA_BENCHES_POST_COMMON_HPP
