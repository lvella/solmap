#!/usr/bin/env python3

# Module to index and access "Atlas Brasileiro de Energia Solar - 2ª Edição",
# available at http://labren.ccst.inpe.br/atlas_2017.html

import os
import dbm
import struct
import array
import datetime
import math

import daytimes
import ephem

dbm_file = os.path.dirname(os.path.realpath(__file__)) + '/../../build/ABES.dbm'

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

    monthly_daytime = array.array('d', [0.0] * 12)

    month = []
    for day in days:
        month.append(day)

        tomorrow = date + datetime.timedelta(days=1)
        if tomorrow.month != date.month:
            m = date.month - 1

            total_daytime = 0.0

            for day in month:
                daylight = day[2]
                # All days must have daylight in this database:
                assert(daylight)

                total_daytime += (daylight[1] - daylight[0]).total_seconds()

            monthly_daytime[m] += total_daytime / len(month)

            month = []

        date = tomorrow

    assert(date == datetime.date(2021, 1, 1))

    # Since this is a Brazilian database, there is no need to
    # check for zeroed monthly daytime.

    # Convert energy to power.
    # Convert Wh to joule and multiply to the number of years simulated:
    print("mean_energy", mean_energy)
    print("monthly_daytime", monthly_daytime)
    mean_power = array.array('d', ((e * 3600.0 * 4.0) / dt for e, dt in zip(mean_energy, monthly_daytime)))
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
    assert(all((math.fabs(k - fk) < 1e-8 for k, fk in zip(key, float_key))))

    mean_energy = [record[month] for month in month_keys]
    return (key, mean_energy)

def convert_to_power(args):
    key, dirn, diff = args
    coords = key2coords(key)

    obs = ephem.Observer()
    obs.lat = str(coords[0])
    obs.lon = str(coords[1])
    obs.elevation = 0

    start_date = datetime.date(2017, 1, 1)

    days = list(daytimes.daytimes_over_range_at(obs,
        datetime.datetime.combine(start_date, datetime.time()),
        4*365+1
    ))

    pdir = energy2power(start_date, days, dirn)
    pdif = energy2power(start_date, days, diff)

    return key, pdir, pdif

def create_indexed_database(direct_normal_dbf, diffuse_dbf):
    from dbfread import DBF
    import multiprocessing.pool

    storage = IndexedStorage(dbm_file, 'n')

    dirnorm = dict(map(extract_row_data, DBF(direct_normal_dbf)))
    diffuse = dict(map(extract_row_data, DBF(diffuse_dbf)))

    assert(len(dirnorm) == len(diffuse))

    pool = multiprocessing.pool.Pool()

    read_data = ((key, dirn, diffuse[key]) for key, dirn in dirnorm.items())

    i = 1
    for key, pdir, pdif in pool.imap_unordered(convert_to_power, read_data):
        storage[key] = (pdir, pdif)

        print(i, '/', len(dirnorm))
        print(key, key2coords(key))
        print(pdir)
        print(pdif)
        print('\n')
        i += 1

if __name__ == '__main__':
    import sys
    create_indexed_database(sys.argv[1], sys.argv[2])
