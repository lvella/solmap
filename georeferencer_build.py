#!/usr/bin/env python3

from cffi import FFI
ffibuilder = FFI()

ffibuilder.set_source("gps_converter", r"""
    #include <math.h>
    #include <wgs84.h>

    void gps_to_euclidean(size_t num_pts, long double *input, long double *output)
    {
        const long double to_rad = M_PI / 180.0l;

        size_t i;
        for(i = 0; i < num_pts; ++i) {
            wgs84_to_euclidean(
                input[i*3] * to_rad,
                input[i*3 + 1] * to_rad,
                input[i*3 + 2],
                &output[i*3]
            );
        }
    }
    """,
    extra_objects=['build/libwgs84.a'], include_dirs=['external/libwgs84/src'])

ffibuilder.cdef("""
    void gps_to_euclidean(size_t num_pts, long double *input, long double *output);
""")

if __name__ == "__main__":
    ffibuilder.emit_c_code("build/gps_converter.c")
