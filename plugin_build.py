#!/usr/bin/env python3
import os
import cffi
ffibuilder = cffi.FFI()

with open('src-host/sun_position.h') as f:
    data = ''.join([line for line in f if not line.startswith('#')])
    ffibuilder.embedding_api(data)

ffibuilder.set_source("sun_position", r'''
    #include "sun_position.h"
''')

script_dir = os.path.dirname(os.path.realpath(__file__))

ffibuilder.embedding_init_code("""
    from sun_position import ffi

    import sys
    sys.path.append('{}/resources/pylib')

    from suntracking import position_over_year

    alive_year_angles = set()

    @ffi.def_extern()
    def create_pos_over_year(*args, **kwargs):
        iter = ffi.new_handle(position_over_year(*args, **kwargs))
        alive_year_angles.add(iter)

        return iter

    @ffi.def_extern()
    def destroy_pos_over_year(iter):
        alive_year_angles.discard(iter)

    @ffi.def_extern()
    def next_pos_over_year(iter, ret):
        try:
            ret.az, ret.alt = next(ffi.from_handle(iter))
            return True
        except StopIteration:
            return False
""".format(script_dir))

ffibuilder.emit_c_code("build/sun_position.c")
