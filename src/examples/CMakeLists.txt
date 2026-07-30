add_executable(hello-world hello-world.c)
add_executable(cv-hello-world cv-hello-world.c)
add_executable(cv-test cv-test.c)
add_executable(deadly-embrace deadly-embrace.c)
add_executable(fifo-example fifo.cpp)
add_executable(producer-consumer producer-consumer.c)
add_executable(producer-consumer-exit producer-consumer-exit.c)
add_executable(producer-consumer-park producer-consumer-park.c)
add_executable(exit-stress exit-stress.c)
add_executable(cv-producer-consumer cv-producer-consumer.c)
target_link_libraries(hello-world PUBLIC -pthread)
target_link_libraries(cv-hello-world PUBLIC -pthread)
target_link_libraries(cv-test PUBLIC -pthread)
target_link_libraries(deadly-embrace PUBLIC -pthread)
target_link_libraries(producer-consumer PUBLIC -pthread)
target_link_libraries(producer-consumer-exit PUBLIC -pthread)
target_link_libraries(producer-consumer-park PUBLIC -pthread)
target_link_libraries(exit-stress PUBLIC -pthread)
target_link_libraries(cv-producer-consumer PUBLIC -pthread)

# Same as producer-consumer-safe-tsan, but worker threads terminate via an
# explicit pthread_exit() call instead of returning -- deliberately WITHOUT
# a pthread_exit wrap yet, to test whether that path has the same
# libtsan-loads-ahead-of-libmcmini load-order bypass already fixed for
# pthread_join/pthread_cond_wait (see those files' header comments). Our own
# pthread_exit is currently defined inside libmcmini.so itself
# (src/lib/interception.c), not compiled into the target the way join/
# cond_wait are, which is exactly the pattern that bypass needs.
add_executable(producer-consumer-exit-tsan producer-consumer-exit.c
                                           ../lib/pthread_join_wrap.c)
target_compile_options(producer-consumer-exit-tsan PUBLIC -fsanitize=thread)
target_link_options(producer-consumer-exit-tsan PUBLIC -fsanitize=thread
                                                        -Wl,--wrap=pthread_join)
target_link_libraries(producer-consumer-exit-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET producer-consumer-exit-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:producer-consumer-exit-tsan>
          ${CMAKE_BINARY_DIR}/producer-consumer-exit-tsan)

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

# Stress test for pthread_exit() robustness at higher thread counts (see
# exit-stress.c) -- same build recipe as producer-consumer-exit-tsan above.
add_executable(exit-stress-tsan exit-stress.c
                                ../lib/pthread_join_wrap.c)
target_compile_options(exit-stress-tsan PUBLIC -fsanitize=thread)
target_link_options(exit-stress-tsan PUBLIC -fsanitize=thread
                                            -Wl,--wrap=pthread_join)
target_link_libraries(exit-stress-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET exit-stress-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:exit-stress-tsan>
          ${CMAKE_BINARY_DIR}/exit-stress-tsan)

# Same build recipe as exit-stress-tsan above. Reliably (4/4 attempts)
# reproduces a genuine checkpoint-thread/TSAN-background-thread startup
# race, distinct from the restoreBuf={0,0} bug this was originally meant to
# help isolate -- see exit-stress-noop.c's own header comment.
add_executable(exit-stress-noop-tsan exit-stress-noop.c
                                     ../lib/pthread_join_wrap.c)
target_compile_options(exit-stress-noop-tsan PUBLIC -fsanitize=thread)
target_link_options(exit-stress-noop-tsan PUBLIC -fsanitize=thread
                                                 -Wl,--wrap=pthread_join)
target_link_libraries(exit-stress-noop-tsan PUBLIC -pthread libmcmini)

add_custom_command(TARGET exit-stress-noop-tsan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:exit-stress-noop-tsan>
          ${CMAKE_BINARY_DIR}/exit-stress-noop-tsan)
