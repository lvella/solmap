#!/usr/bin/env python3

# Module to index and access "Atlas Brasileiro de Energia Solar - 2ª Edição",
# available at http://labren.ccst.inpe.br/atlas_2017.html

import os
import dbm
import struct
import array
import datetime

import daytimes
import ephem
import numpy as np

dbm_file = os.path.dirname(os.path.realpath(__file__)) + '/../../build/ABES.dbm'

# This function maps latitude/longitude coordinate space to a coordinate space where
# integer values matches the values stored in the database. Integer values
# in new coordinate space is used as key entries in the hash indexed
# database.
def coords2key(latitude, longitude):
    latitude = 10.0 * latitude - 0.995
    longitude = 10.0 * longitude - 0.49
    return (latitude, longitude)

# Interface to DBM database handling the data encoding.
class IndexedStorage:
    def __init__(self, filename, mode='r'):
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

def energy2power(date, days, mean_energy):
    assert(len(days) == 4*365+1)

    A = np.zeros([12,12])

    month = []
    for day in days:
        month.append(day)

        tomorrow = date + datetime.timedelta(days=1)
        if tomorrow.month != date.month:
            m = date.month - 1
            Ms = month[0][0]
            tm = (month[-1][1] - Ms).total_seconds()

            Pc = 0.0
            Pn = 0.0

            for day in month:
                daylight = day[2]
                if not daylight:
                    continue

                Ds = (daylight[0] - Ms).total_seconds()
                De = (daylight[1] - Ms).total_seconds()
                dif = De - Ds
                add = Ds + De

                Pc += dif * (2.0 - add / tm)
                Pn += dif * add / tm

            factor = 0.5 / len(month)

            A[m,m] += factor * Pc
            A[m,(m+1)%12] += factor * Pn

            month = []

        date = tomorrow

    assert(date == datetime.date(2021, 1, 1))

    # Since this is a Brazilian database, there is no need to check
    # for zeroed rows (3 months in a row without daylight).

    # 4 years times energy converted to joule (times 3600).
    mean_power = np.linalg.solve(A, 4.0 * 3600.0 * np.array(mean_energy, dtype=float))
    return mean_power

month_keys = [
        '01_JAN',
        '02_FEB',
        '03_MAR',
        '04_APR',
        '05_MAY',
        '06_JUN',
        '07_JUL',
        '08_AUG',
        '09_SEP',
        '10_OCT',
        '11_NOV',
        '12_DEZ'
]

def extract_row_data(record):
    float_key = coords2key(record['LAT'], record['LON'])
    key = tuple(map(round, float_key))

    mean_energy = [record[month] for month in month_keys]
    return (key, mean_energy)

def create_indexed_database(direct_normal_dbf, diffuse_dbf):
    from dbfread import DBF

    storage = IndexedStorage(dbm_file, 'n')

    dirnorm = dict(map(extract_row_data, DBF(direct_normal_dbf)))
    diffuse = dict(map(extract_row_data, DBF(diffuse_dbf)))

    for key in dirnorm:
        obs = ephem.Observer()
        obs.lat = str(key[0])
        obs.lon = str(key[1])
        obs.elevation = 0

        start_date = datetime.date(2017, 1, 1)

        days = list(daytimes.daytimes_over_range(obs,
            datetime.datetime.combine(start_date, datetime.time()),
            4*365+1
        ))

        pdir = energy2power(start_date, days, dirnorm[key])
        pdif = energy2power(start_date, days, diffuse[key])

        storage[key] = (pdir, pdif)

        print(key)
        print(pdir)
        print(pdif)
        print('\n')

if __name__ == '__main__':
    import sys
    create_indexed_database(sys.argv[1], sys.argv[2])
