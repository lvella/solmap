#!/usr/bin/env python3
import cffi
ffibuilder = cffi.FFI()

with open('src/sun_position.h') as f:
    data = ''.join([line for line in f if not line.startswith('#')])
    ffibuilder.embedding_api(data)

ffibuilder.set_source("sun_position", r'''
    #include "sun_position.h"
''')

ffibuilder.embedding_init_code("""
    from sun_position import ffi

    import ephem
    import datetime

    def position_over_year(latitude, longitude, elevation):
        '''
        Yields the position (azimuth, altitude) of the sun over the year
        of 2017, for every 5 minutes of daylight.

        Result is a tuple.
        '''

        obs = ephem.Observer()
        obs.lat = str(latitude)
        obs.lon = str(longitude)
        obs.elevation = elevation

        dt = datetime.timedelta(minutes=5)

        t = datetime.datetime(2017, 1, 1, 0, 0, 0)

        final = datetime.datetime(2018, 1, 1, 0, 0, 0)

        while(t <= final):
            obs.date = t.strftime('%Y/%m/%d %H:%M:%S')
            v = ephem.Sun(obs)

            if v.alt > 0.0:
                #y = math.sin(v.alt)
                #compl = math.cos(v.alt)
                #x = math.sin(v.az) * compl
                #z = math.cos(v.az) * compl

                #yield (x, y, z)
                yield (v.az, v.alt)

            t += dt

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
""")

ffibuilder.emit_c_code("build/sun_position.c")
