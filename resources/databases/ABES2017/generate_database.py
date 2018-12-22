#!/usr/bin/env python3

# This script generates the ABES.dbm file from the original "Atlas Brasileiro
# de Energia Solar - 2ª Edição" database
# (available at http://labren.ccst.inpe.br/atlas_2017.html). Generated file is
# used by solmap to quickly query the solar incidence over the year for a given
# latidude and longitue. Requires dbfread module and takes a looong time to run.

import os
import sys

# Setup import path
sys.path.append(os.path.dirname(os.path.realpath(__file__)) + '/../../pylib')

import array
import math
import ephem
import datetime
import daytimes
import ABES2017 as ABES

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
    float_key = ABES.coords2key(record['LAT'], record['LON'])
    key = tuple(map(round, float_key))
    assert(all((math.fabs(k - fk) < 1e-8 for k, fk in zip(key, float_key))))

    mean_energy = [record[month] for month in month_keys]
    return (key, mean_energy)

def convert_to_power(args):
    key, dirn, diff = args
    coords = ABES.key2coords(key)

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

    storage = ABES.IndexedStorage(ABES.dbm_file, 'n')

    dirnorm = dict(map(extract_row_data, DBF(direct_normal_dbf)))
    diffuse = dict(map(extract_row_data, DBF(diffuse_dbf)))

    assert(len(dirnorm) == len(diffuse))

    pool = multiprocessing.pool.Pool()

    read_data = ((key, dirn, diffuse[key]) for key, dirn in dirnorm.items())

    i = 1
    for key, pdir, pdif in pool.imap_unordered(convert_to_power, read_data):
        storage[key] = (pdir, pdif)

        print(i, '/', len(dirnorm))
        print(key, ABES.key2coords(key))
        print(pdir)
        print(pdif)
        print('\n')
        i += 1

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("""
Error: missing parameters.

Usage:
    {} <path to direct_normal_means.dbf> <path to diffuse_means.dbf>

You can download DIRECT_NORMAL.zip and DIFFUSE.zip from:
 http://labren.ccst.inpe.br/atlas_2017.html
""".format(sys.argv[0]))
    elif os.path.isfile(ABES.dbm_file):
        print("""
Error: database file already exists.\n
If you are sure you want to regenerate the database,
please delete the following file and try again:
 {}
""".format(ABES.dbm_file))
    else:
        create_indexed_database(sys.argv[1], sys.argv[2])
