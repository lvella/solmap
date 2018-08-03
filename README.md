# Solar Map

A command line tool to evaluate the best placement position of solar
panels, given a 3-D model of the place, the latitude and longitude.

All file formats for 3-D models supported by [Assimp][1] can be used.
I conventioned +Y axis as up, and -Z as north, using a right hand
coordinate system, which gives +X as east (see 3-D model `reference.obj`).
An execution example for a floating cube over a flat surface replacing
Uberlândia's town hall is given by:

`$ ./build/solmap -18.9118465 -48.2560091 sample-models/hover-box.stl`

The command will output the optimal angle for solar panel placement, as
well as a solar incidence map overlayed to the 3-D model, in a file named
`incidence.vtk`. This file can be opened by scientific visualization tools
like [Paraview][3] and [VisIt][2], both which are free software.

The program works by computing the sun's position for every 5 minutes of
daytime over the year of 2017. For each calculated position, it accumulates
the solar incidence over every exposed vertex of the 3-D model, considering
the surface direction (given by normal vector of the point) and the shadows
cast by the model.

Most of the work is performed by the GPU, using the Vulkan API. By using
GPU's specialized hardware for graphics rendering and massively parallel
computation, the results can be computed very quickly in a cheap computer.

You may notice the azimuth suggested by the program for solar panel
placement is not exactly the theoretical value expected for your latitude
(it should be either 0° or 180°, depending whether the place is in the
southern or northern hemisphere), but there is a small error of less than a
tenth of a degree. This seems to be due the time discretization error, because
a day is not necessarily started with sun's altitude at zero, but at anywhere
from zero to 5 minutes from that point.

## Dependencies

To build, you need:
 - Python 3 with CFFI, for Python interface;
 - GLM library, for linear algebra;
 - glslc command line tool, from shaderc package, to compile GLSL shaders
into Vulkan's SPIR-V bytecode;
 - Boost.Functional/Hash, for hash combining.

To run, you need:
 - Python 3 with CFFI;
 - PyEphem on your Python path, for astronomical sun positioning;
 - Vulkan library, for GPU access;
 - Assimp library, to load 3D models.

[1]: https://github.com/assimp/assimp
[2]: https://wci.llnl.gov/simulation/computer-codes/visit/downloads
[3]: https://www.paraview.org/
