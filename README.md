SheepShear -- A fork of SheepShaver
===================================

While SheepShaver provides decent PowerPC emulation
on Linux, BeOS, and Windows, it development has been
extremely slow in the last 5 plus years.

To ensure a quality MacOS Classic emulator continues
on to exist in a functioning state for Linux and Haiku,
SheepShear was created as a fork of the SheepShaver codebase

Targeted Platforms:
-------------------
 * Haiku
 * Linux
 * MacOS X
   - MacOS X moved to clang in recent versions. The old qemu
     dyngen engine we use doesn't play nicely with clang :(
 * Windows ?

TODO Items:
-----------
 * Refactor code base, clean up extern functions to classes
 * Better GUI
 * Better Icons
 * Easier to use

Done Items:
-----------
 * Basilisk II code integrated
 * Fork renamed to SheepShear


Frequently asked questions:
===========================

SheepShear is based on SheepShaver... What is SheepShaver?
 - SheepShaver is an excellent PowerPC emulator found here:
   http://sheepshaver.cebix.net
   
Why SheepShear?
 - SheepShear is an synonym of SheepShaver just as
   SheepShaver was an pun of ShapeShifter

Where can I converse in a public place on this fine product?
 - IRC
   irc.freenode.net #sheepshear

How do I compile SheepShear?
 - See COMPILE for instructions

I'm getting "Cannot map Low memory Globals: Operation not permitted"
 - This is due to newer linux kernels preventing non-root
   applications from mapping lower memory segments (security?)
   to solve, set the kernel paramater (via sysctl or sysctl.conf)

   vm.mmap_min_addr = 0

License
=======

SheepShear is licensed as GPLv2 just as SheepShaver.
