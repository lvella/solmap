#!/usr/bin/env python3

# Module to index and access "Atlas Brasileiro de Energia Solar - 2ª Edição",
# available at http://labren.ccst.inpe.br/atlas_2017.html

import os
import struct
import dbm

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
        assert(len(value) == 12*4*2)

        direct_normal = array.array('f')
        direct_normal.frombytes(value[:48])

        diffuse = array.array('f')
        diffuse.frombytes(value[48:])

        return direct_normal, diffuse

    def __enter__(self):
        pass

    def __exit__(*args):
        del self.db

    @staticmethod
    def encode_key(key):
        return struct.pack('hh', *key)

# Returns the mean irration per month in a given location:
def get_montly_irradiation(latitude, longitude):
    pass # TODO: implement
