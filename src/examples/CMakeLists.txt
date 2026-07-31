add_executable(hello-world hello-world.c)
add_executable(cv-hello-world cv-hello-world.c)
add_executable(cv-test cv-test.c)
add_executable(deadly-embrace deadly-embrace.c)
add_executable(fifo-example fifo.cpp)
add_executable(producer-consumer producer-consumer.c)
add_executable(producer-consumer-park producer-consumer-park.c)
add_executable(cv-producer-consumer cv-producer-consumer.c)
target_link_libraries(hello-world PUBLIC -pthread)
target_link_libraries(cv-hello-world PUBLIC -pthread)
target_link_libraries(cv-test PUBLIC -pthread)
target_link_libraries(deadly-embrace PUBLIC -pthread)
target_link_libraries(producer-consumer PUBLIC -pthread)
target_link_libraries(producer-consumer-park PUBLIC -pthread)
target_link_libraries(cv-producer-consumer PUBLIC -pthread)

# producer-consumer-park.c never calls pthread_join() or pthread_exit() --
# main returns while both workers are still alive, parked forever on an
# unposted semaphore (see that file's own comment, modeled on
# multithreaded-fork-tsan-2.0's test_park.c). No --wrap needed: neither
# pthread_join nor pthread_cond_wait is ever called here.
add_executable(producer-consumer-park-tsan producer-consumer-park.c)
target_compile_options(producer-consumer-park-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-park-tsan PUBLIC -fsanitize=thread)
target_link_libraries(producer-consumer-park-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET producer-consumer-park-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-park-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-park-tsan)
