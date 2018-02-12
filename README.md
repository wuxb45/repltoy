To build a binary for debugging:

    $ make

To build a binary for performance test:

    $ make O=r

To run the replacement algorithm simulation (first you need to prepare a tracefile):

    $ ./replay.out lru tracefile 1.0 32

Parameters:

[1] the name of the replacement algorithm. Now we only support lru.
[2] the name of a trace file.
[3] sampling rate. a number in [0.0, 1.0]
[4] number of probes. This controls how many different cache sizes will be used.


To generate a trace file of 10000 accesses:

    $ ./tracegen.out 10000 >mytrace

To change how the trace is generated you can look into tracegen.c for more details.

Please report bugs at https://github.com/wuxb45/repltoy/issues.
