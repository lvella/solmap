# Solar Map

Right now, this is only a command line tool able to find the best
placement position for a solar panel, given the latitude and longitude.

It does that by averaging the sun's position at every 5 minutes of
daytime over the year of 2017, and assuming 2017 is representative of
other years.

I don't know why it suggests a non-zero azimuth on most places (it
should be either 0° or 180°, depending whether the place is in the
southern or northern hemisphere). Possibly it is a time discretization
error, because I don't necessarily start a day with sun's altitude
at zero, but at anywhere from zero to 5 minutes from that point.

This could be a much simpler Python script, instead of this convoluted
C++ threaded program, but this is a work in progress. Eventually, it
will use Vulkan GPU interface to calculate the solar incidence over the
surface of the 3D model provided as input.

## Dependencies

To build, you need:
 - Python 3 with CFFI, for Python interface;
 - GLM library, for linear algebra.

To run, you need:
 - Python 3 with CFFI;
 - PyEphem on your Python path, for astronomical sun positioning;
 - Vulkan, for GPU access;
 - Assimp, to load 3D models.
