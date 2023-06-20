#include <fiona/io_context.hpp>
#include <fiona/sleep.hpp>

#include <boost/core/lightweight_test.hpp>

#include <iostream>

static int num_runs = 0;

template <class Rep, class Period> struct duration_guard {
  std::chrono::duration<Rep, Period> expected;
  std::chrono::system_clock::time_point prev;

  duration_guard(std::chrono::duration<Rep, Period> expected_)
      : expected(expected_), prev(std::chrono::system_clock::now()) {}

  ~duration_guard() {
    auto now = std::chrono::system_clock::now();

    auto elapsed =
        std::chrono::duration_cast<std::chrono::duration<Rep, Period>>(now -
                                                                       prev);
    BOOST_TEST_GE(elapsed, expected);
    BOOST_TEST_LT(elapsed, expected * 1.05);
  }
};

namespace {

template <class Rep, class Period>
fiona::task
sleep_coro(fiona::executor ex, std::chrono::duration<Rep, Period> d) {
  {
    duration_guard guard(d);
    auto ec = co_await sleep_for(ex, d);
    BOOST_TEST(!ec);
  }

  ++num_runs;
  co_return;
}

fiona::task
nested_sleep_coro(fiona::executor ex) {
  {
    std::chrono::milliseconds d(1500);
    duration_guard guard(d);
    auto ec = co_await sleep_for(ex, d);
    BOOST_TEST(!ec);
  }

  {
    std::chrono::milliseconds d(750);
    duration_guard guard(d);
    co_await sleep_coro(ex, d);
  }

  ++num_runs;
  co_return;
}

fiona::task
nested_sleep_coro_late_return(fiona::executor ex) {
  {
    std::chrono::milliseconds d(1500);
    duration_guard guard(d);
    co_await sleep_coro(ex, d);
  }

  {
    std::chrono::milliseconds d(2500);
    duration_guard guard(d);
    auto ec = co_await sleep_for(ex, d);
    BOOST_TEST(!ec);
  }

  ++num_runs;
}

fiona::task
empty_coroutine(fiona::executor) {
  ++num_runs;
  co_return;
}

fiona::task
nested_post_timer(fiona::executor ex) {
  ex.post(nested_sleep_coro(ex));
  ex.post(nested_sleep_coro_late_return(ex));

  ++num_runs;
  co_return;
}

fiona::task
recursion_test(fiona::executor ex, int n) {
  if (n == 0) {
    ++num_runs;
    co_return;
  }

  {
    std::chrono::milliseconds d(100);
    duration_guard guard(d);
    auto ec = co_await sleep_for(ex, d);
    BOOST_TEST(!ec);
  }

  co_await recursion_test(ex, n - 1);
}

void
test1() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(sleep_coro(ex, std::chrono::milliseconds(600)));
  ioc.run();
  BOOST_TEST_EQ(num_runs, 1);
}

void
test2() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(nested_sleep_coro(ex));
  ioc.run();
  BOOST_TEST_EQ(num_runs, 2);
}

void
test3() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(nested_sleep_coro_late_return(ex));
  ioc.run();
  BOOST_TEST_EQ(num_runs, 2);
}

void
test4() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(empty_coroutine(ex));
  ioc.run();
  BOOST_TEST_EQ(num_runs, 1);
}

void
test5() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(sleep_coro(ex, std::chrono::milliseconds(235)));
  ioc.post(nested_sleep_coro(ex));
  ioc.post(nested_sleep_coro_late_return(ex));
  ioc.post(empty_coroutine(ex));
  {
    duration_guard guard(std::chrono::milliseconds(1500 + 2500));
    ioc.run();
  }
  BOOST_TEST_EQ(num_runs, 1 + 2 + 2 + 1);
}

void
test6() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  ioc.post(nested_post_timer(ex));
  ioc.run();
  BOOST_TEST_EQ(num_runs, 1 + 2 + 2);
}

void
test7() {
  std::cout << __func__ << std::endl;
  num_runs = 0;
  fiona::io_context ioc;
  auto ex = ioc.get_executor();
  for (int i = 0; i < 10; ++i) {
    ioc.post(recursion_test(ex, 20));
  }
  ioc.run();
  BOOST_TEST_EQ(num_runs, 10);
}

} // namespace

int
main() {
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  test7();
  return boost::report_errors();
}
