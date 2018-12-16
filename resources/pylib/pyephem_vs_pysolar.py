#!/usr/bin/env python3

import daytimes
import datetime
import ephem
import pysolar.solar
import time
import math
import random

def random_pos():
    while True:
        p = [random.uniform(-1.0, 1.0) for i in range(3)]
        norm = (p[0]**2 + p[1]**2 + p[2]**2)**0.5
        if norm <= 1.0:
            break

    lon = math.atan2(p[1], p[0]) * 180.0 / math.pi
    lat = 90.0 - math.acos(p[2] / norm) * 180.0 / math.pi

    return lat, lon

def ephem_altitude_eval(lat, lon):
    obs = ephem.Observer()
    obs.lat = str(lat)
    obs.lon = str(lon)
    obs.elevation = 0

    def ret(ref, x):
        time = ref + datetime.timedelta(seconds=x)
        return daytimes.sun_pos(obs, time)[1]

    return ret

def pysolar_altitude_eval(lat, lon):
    def ret(ref, x):
        time = ref + datetime.timedelta(seconds=x)
        return pysolar.solar.get_altitude(lat, lon, time)

    return ret

def test():
    ephem_time = 0.0
    pysolar_time = 0.0

    max_dif = 0.0
    l2_sum = 0.0
    def update_dif(a, b):
        nonlocal max_dif, l2_sum
        d = math.fabs((a - b).total_seconds())
        if d > max_dif:
            max_dif = d
        l2_sum += d * d

    for i in range(1000):
        lat, lon = random_pos()
        print("# {} #:\n  coords: {}, {}".format(i, lat, lon))

        a = time.time()
        ephem_data = list(daytimes.daytimes_over_range(ephem_altitude_eval(lat, lon), lon))
        b = time.time()
        pysolar_data = list(daytimes.daytimes_over_range(pysolar_altitude_eval(lat, lon), lon))
        c = time.time()

        ephem_time += b - a
        pysolar_time += c - b

        for eph, sol in zip(ephem_data, pysolar_data):
            for j in range(2):
                update_dif(eph[j], sol[j])

        print("  accumulated ephem time: {}\n  accumulated pysolar time: {}\n  Linf norm: {}\n  L2 norm: {}\n\n".format(ephem_time, pysolar_time, max_dif, l2_sum**0.5))

        max_dif = 0.0
        l2_sum = 0.0

if __name__ == '__main__':
    test()
