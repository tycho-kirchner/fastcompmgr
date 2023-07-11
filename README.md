# fastcompmgr

__fastcompmgr__ is a _fast_ compositor for X, a fork of an early version
of __Compton__ which is a fork of __xcompmgr-dana__ which is a fork
of __xcompmgr__.

I used to use good old xcompmgr for long, because compton always
felt a bit laggy when moving/resizing windows or kinetic-scrolling
a webpage. Having tested the latest picom-10.2, it seems, things got even
worse. However, xcompmgr does not draw shadows on argb windows (e.g.
some terminals) and
has several other glitches. That's why I traveled back into 2011, where
this feature was just added, cherry picked some later compton commits
to get rid of spurious segfaults and memleaks and made that version even
faster, based on profiling.
For example, window move- and resize events are limited in their
event-count and rendered at a somewhat fixed framerate, while
scrolling is still done as fast as possible. Occluded windows are not
painted and memory allocations/deallocations are largely avoided,
allowing for faster repaints of the screen.
On the downside, fading is currently broken (I don't use it). Sorry
for that (;


## Building

The same dependencies as xcompmgr.

### Dependencies:

* libx11
* libxcomposite
* libxdamage
* libxfixes
* libxrender
* pkg-config
* make

To build, make sure you have the above dependencies:

``` bash
$ make
$ make install
```

## Usage

``` bash
$ fastcompmgr -o 0.4 -r 12 -c -C
```

## License

xcompmgr has gotten around. As far as I can tell, the lineage for this
particular tree is something like:

* Keith Packard (original author)
* Matthew Hawn
* ...
* Dana Jansens
* Christopher Jeffrey
* Tycho Kirchner

Not counting the tens of people who forked it in between.

See LICENSE for more info.
