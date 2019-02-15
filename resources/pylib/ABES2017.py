#!/usr/bin/env python3

# Module to index and access "Atlas Brasileiro de Energia Solar - 2ª Edição",
# available at http://labren.ccst.inpe.br/atlas_2017.html

import os
import struct
import dbm
import math
import numpy as np
import scipy.interpolate

from daytimes import OutOfDatabaseDomain

dbm_file = os.path.abspath(
    os.path.dirname(os.path.realpath(__file__)) + '/../databases/ABES2017/ABES.dbm'
)

# This function maps latitude/longitude coordinate space to a coordinate space
# where integer values matches the values stored in the database. Integer values
# in new coordinate space is used as key entries in the hash indexed database.
def coords2key(latitude, longitude):
    latitude = 10.0 * (latitude - 0.0995)
    longitude = 10.0 * (longitude - 0.051)
    return (latitude, longitude)

# Inverse operation:
def key2coords(key):
    latitude = (key[0] / 10.0) + 0.0995
    longitude = (key[1] / 10.0) + 0.051
    return latitude, longitude

# Interface to DBM database handling the data encoding.
class IndexedStorage:
    def __init__(self, filename=dbm_file, mode='r'):
        self.db = dbm.open(filename, mode)

    def __setitem__(self, key, data):
        direct_normal, diffuse = data
        assert(len(direct_normal) == 12)
        assert(len(diffuse) == 12)

        self.db[self.encode_key(key)] = (
            direct_normal.tobytes() +
            diffuse.tobytes()
        )

    def __getitem__(self, key):
        value = self.db[self.encode_key(key)]
        assert(len(value) == 12*8*2)

        ret = np.frombuffer(value)
        ret.shape = (2, 12)
        return ret

    def __enter__(self):
        return self

    def __exit__(self, *args):
        del self.db

    @staticmethod
    def encode_key(key):
        return struct.pack('hh', *key)

# 2-D bilinear interpolation, used when 4 points are available
def bilinear2d(p, l, sw, se, ne, nw):
    w2e = (p[1] - l[1])
    n = nw + w2e * (ne - nw)
    s = sw + w2e * (se - sw)

    s2n = (p[0] - l[0])
    return s + s2n * (n - s)

def plane_intersect(p, orig, a, b):
    # Normal to the plane
    nx = a[1] * b[2] - a[2] * b[1]
    ny = a[2] * b[0] - a[0] * b[2]
    nz = a[0] * b[1] - a[1] * b[0]

    # Z value where point p crosses the plane:
    return orig[2] - (nx * (p[0] - orig[0]) + ny * (p[1] - orig[1])) / nz

# 2-D linear interpolation, used when 3 points are available.
def linear2d(p, x, y, z):
    # First vector on plane
    a = [
        x[1] - x[0],
        y[1] - y[0],
        z[1] - z[0]
    ]

    # Second vector on plane
    b = [
        x[2] - x[0],
        y[2] - y[0],
        z[2] - z[0]
    ]

    return plane_intersect(p, [x[0], y[0], z[0]], a, b)

# Closest point to 1-D linear interpolation. Used when 2 points are available.
def projectlinear1d(p, x, y, z):
    # First vector on plane
    a = [
        x[1] - x[0],
        y[1] - y[0],
        z[1] - z[0]
    ]

    # Second vector on plane, perpendicular to the first and
    # contained on xy-plane.
    b = [-a[1], a[0], 0.0]

    return plane_intersect(p, [x[0], y[0], z[0]], a, b)

# Returns the mean irration per month in a given location:
def get_montly_incidence(latitude, longitude):
    # Convert latitude and longitude to key space
    fk = coords2key(latitude, longitude)

    # Get the 4 known points surrounding the given point:
    l = tuple(map(math.floor, fk))
    h = tuple(map(math.ceil, fk))

    # Retrive the incidence from the database:
    with IndexedStorage() as storage:
        x = []
        y = []
        z = []
        for p in (l, (l[0], h[1]), h, (h[0], l[1])):
            try:
                val = storage[p]

                x.append(p[0])
                y.append(p[1])
                z.append(val)
            except KeyError:
                pass

        if len(x) == 4:
            ret = bilinear(fk, l, *z)
        elif len(x) == 3:
            ret = linear2d(fk, x, y, z)
        elif len(x) == 2:
            ret = projectlinear1d(fk, x, y, z)
        elif len(x) == 1:
            ret = z[0]
        else:
            raise OutOfDatabaseDomain()

    return tuple(zip(ret[0], ret[1]))

if __name__ == '__main__':
    import sys
    print(get_montly_incidence(float(sys.argv[1]), float(sys.argv[2])))
