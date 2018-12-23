#!/usr/bin/env python3

# Module to index and access "Atlas Brasileiro de Energia Solar - 2ª Edição",
# available at http://labren.ccst.inpe.br/atlas_2017.html

import os
import struct
import dbm
import math
import numpy as np
import scipy.interpolate

dbm_file = os.path.abspath(
    os.path.dirname(os.path.realpath(__file__)) + '/../databases/ABES2017/ABES.dbm'
)

# This function maps latitude/longitude coordinate space to a coordinate space where
# integer values matches the values stored in the database. Integer values
# in new coordinate space is used as key entries in the hash indexed
# database.
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

# Returns the mean irration per month in a given location:
def get_montly_incidence(latitude, longitude):
    # Convert latitude and longitude to key space
    fk = coords2key(latitude, longitude)

    # Get the 4 known points surrounding the given point:
    l = tuple(map(math.floor, fk))
    h = tuple(map(math.ceil, fk))

    # Retrive the incidence from the database:
    with IndexedStorage() as storage:
        sw = storage[l]
        se = storage[l[0], h[1]]
        ne = storage[h]
        nw = storage[h[0], l[1]]

    w2e = (fk[1] - l[1])
    n = nw + w2e * (ne - nw)
    s = sw + w2e * (se - sw)

    s2n = (fk[0] - l[0])
    ret = s + s2n * (n - s)

    return tuple(zip(ret[0], ret[1]))

if __name__ == '__main__':
    import sys
    print(get_montly_incidence(float(sys.argv[1]), float(sys.argv[2])))
