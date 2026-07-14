#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

bool DEBUG_MODE = true;
struct PhilosopherForks {
  int id;
  std::mutex &left_fork;
  std::mutex &right_fork;
};

void philosopher_doit(int id, std::mutex &left_fork, std::mutex &right_fork) {
  // Simulate thinking/arrival
  //   std::this_thread::sleep_for(std::chrono::seconds(1));

  // Naive acquisition: pick up left then right (leads to deadlock)
  left_fork.lock();
  if (DEBUG_MODE)
    std::cout << "Philosopher " << id << " picked up left fork.\n";

  //   std::this_thread::sleep_for(std::chrono::seconds(1));

  right_fork.lock();
  if (DEBUG_MODE)
    std::cout << "Philosopher " << id
              << " picked up right fork and is eating.\n";

  // Simulate eating
  //   std::this_thread::sleep_for(std::chrono::seconds(2));

  // Release forks
  right_fork.unlock();
  left_fork.unlock();

  if (DEBUG_MODE)
    std::cout << "Philosopher " << id
              << " finished eating and put down forks.\n";
}

int main() {
  const int NUM_PHILOSOPHERS = 3;

  // std::mutex is not copyable, so we use a vector of unique_ptrs
  // or a fixed-size array to keep them stable in memory.
  std::vector<std::mutex> forks(NUM_PHILOSOPHERS);
  std::vector<std::thread> threads;

  for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
    // We pass references to the mutexes in the vector
    threads.emplace_back(philosopher_doit, i, std::ref(forks[i]),
                         std::ref(forks[(i + 1) % NUM_PHILOSOPHERS]));
  }

  // Wait for all threads to complete
  for (auto &t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }

  return 0;
}
