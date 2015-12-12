Multichase - a pointer chaser benchmark

1/ BUILD

   - just type:

     $ make

2/ INSTALL

   - just run from current directory or copy multichase wherever you need to

3/ RUN

   - By default, multichase will perform a pointer chase through an array
     size of 256MB and a stride size of 256 bytes for 2.5 seconds on a single
     thread:

     $ multichase

   - Pointer chase through an array of 4MB with a stride size of 64 bytes:

     $ multichase -m 4m -s 64

  - Pointer chase through an array of 1GB for 10 seconds (-n is the number of 0.5  second samples):

     $ multichase -m 1g -n 20

  - Pointer chase through an array of 256KB with a stride size of 128 bytes on 2 threads.
    Thread 0 accesses every 128th byte, thread 1 accesses every 128th byte offset by sizeof(void*)=8
    on 64bit architectures:

    $ multichase -m 256k -s 128 -t 2


   - Pingpong: measure latency of exchanging a line between cores.
     To run, simply do:
    $ pingpong -u

   - Fairness: measure fairness with N threads competing to increment an atomic variable.
     To run, simply do:
    $ fairness
