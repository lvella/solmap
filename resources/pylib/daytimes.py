#!/usr/bin/env python3

import ephem
import datetime
import functools
import math

# Exception thrown if position is not covered by database.
class OutOfDatabaseDomain(Exception):
    pass

# Inverse golden ratio
phi = 2.0 / (1.0 + 5.0**0.5)

# Reference start date
ref_start = datetime.datetime(2017, 1, 1, 0, 0, 0, tzinfo=datetime.timezone.utc)

# Golden section method, straight from Wikipedia page:
def int_minimize(fun, lo, hi):
    a = (lo, fun(lo))
    b = (hi, fun(hi))

    c = int(b[0] - (b[0] - a[0]) * phi)
    c = (c, fun(c))

    d = int(a[0] + (b[0] - a[0]) * phi)
    d = (d, fun(d))

    while a[0]+1 < b[0]:
        if c[1] < d[1]:
            b = d
            d = c

            c = int(b[0] - (b[0] - a[0]) * phi)
            c = (c, fun(c))
        else:
            a = c
            c = d

            d = int(a[0] + (b[0] - a[0]) * phi)
            d = (d, fun(d))

    if a[1] < b[1]:
        return a
    else:
        return b

# Bisection search, assumes fn is a increasing function.
def int_root_find(fn, lo, hi):
    lo, hi = map(int, (lo, hi))
    while lo + 1 < hi:
        m = int((lo + hi)*0.5)

        if fn(m) > 0.0:
            hi = m
        else:
            lo = m

    if math.fabs(fn(lo)) < math.fabs(fn(hi)):
        return lo
    return hi

def get_solar_database(latitude, longitude):
    # TODO: generalize to support multiple databases
    import ABES2017

    try:
        data = ABES2017.get_montly_incidence(latitude, longitude)
        def get_incidence(date):
            nonlocal data
            return data[date.month - 1]
    except OutOfDatabaseDomain:
        print("Warning: Location not covered by database.\nUsing 1000 watts as direct power and 0 as indirect.")
        def get_incidence(date):
            return (1000.0, 0.0)

    return get_incidence

def daytimes_over_range(altitude_func, longitude, ref_start, num_days):
    # Timezone is used to identify which calendar day and
    # month is identified by the solar database, given
    # a date in UTC. This is only important in places near
    # the international date line.
    timezone = longitude / 15.0

    # We start from a reference date we know to be near the
    # lowest sun position at the given day.
    lowest = ref_start + datetime.timedelta(hours=timezone)

    # Find the time of lowest sun position for the date:
    fn = functools.partial(altitude_func, lowest)
    delta, l_alt = int_minimize(fn, -6*3600, 6*3600)
    lowest += datetime.timedelta(seconds=delta)

    for i in range(num_days):
        day_start = lowest

        # Find sun's peak:
        highest = lowest + datetime.timedelta(hours=12)
        fn = lambda x: -altitude_func(highest, x)
        delta, h_alt = int_minimize(fn, -6*3600, 6*3600)
        h_alt = -h_alt
        highest += datetime.timedelta(seconds=delta)

        if l_alt >= 0:
            daytime_start = lowest
        elif h_alt > 0:
            # Find sunrise:
            half_delta = (highest - lowest)*0.5
            sunrise = lowest + half_delta
            fn = functools.partial(altitude_func, sunrise)
            half_delta = half_delta.total_seconds()
            delta = int_root_find(fn, -half_delta, half_delta)
            sunrise += datetime.timedelta(seconds=delta)

            daytime_start = sunrise
        else:
            daytime_start = None

        # Find next lowest position:
        lowest = highest + datetime.timedelta(hours=12)
        fn = functools.partial(altitude_func, lowest)
        delta, l_alt = int_minimize(fn, -6*3600, 6*3600)
        lowest += datetime.timedelta(seconds=delta)

        if l_alt >= 0:
            daytime_finish = lowest
        elif h_alt > 0:
            # Find sundown:
            half_delta = (lowest - highest)*0.5
            sundown = highest + half_delta
            fn = lambda x: -altitude_func(sundown, x)
            half_delta = half_delta.total_seconds()
            delta = int_root_find(fn, -half_delta, half_delta)
            sundown += datetime.timedelta(seconds=delta)

            daytime_finish = sundown

        daytime = (daytime_start, daytime_finish) if daytime_start else None
        yield day_start, lowest, daytime

def sun_pos(obs, time):
    obs.date = time.strftime('%Y/%m/%d %H:%M:%S')
    v = ephem.Sun(obs)

    return v.az, v.alt

def daytimes_over_range_at(obs, ref_start=ref_start, num_days=365):
    def altitude_func(ref, x):
        nonlocal obs
        time = ref + datetime.timedelta(seconds=x)
        return sun_pos(obs, time)[1]

    return daytimes_over_range(altitude_func, float(obs.lon), ref_start, num_days)

def quantize_year(latitude, longitude, elevation, max_dt):
    obs = ephem.Observer()
    obs.lat = str(latitude)
    obs.lon = str(longitude)
    obs.elevation = elevation

    incidence_calculator = get_solar_database(latitude, longitude)

    first_day = ref_start.date()
    for i, (start, finish, daytime) in enumerate(daytimes_over_range_at(obs)):
        if not daytime:
            pass

        direct_power, indirect_power = incidence_calculator(
            first_day + datetime.timedelta(days=i)
        )

        delta = (daytime[1] - daytime[0]).total_seconds()
        n = max(2, int(math.ceil(delta / max_dt)))
        dt = float(delta) / n

        # Start day integration, using trapezoidal rule
        # https://en.wikipedia.org/wiki/Trapezoidal_rule
        az, alt = sun_pos(obs, daytime[0])

        # Coefficient for the first term of trapezoidal rule: dt/2
        yield (dt*0.5, az, alt, direct_power, indirect_power)

        # Middle terms for trapezoidal rule:
        for i in range(1, n):
            az, alt = sun_pos(
                obs,
                daytime[0] + datetime.timedelta(seconds=i*dt)
            )
            yield (dt, az, alt, direct_power, indirect_power)

        # Last term for trapezoidal rule (it uses n+1 points for n chunks):
        az, alt = sun_pos(obs, daytime[1])
        yield (dt*0.5, az, alt, direct_power, indirect_power)

if __name__ == '__main__':
    import sys
    for e in quantize_year(sys.argv[1], sys.argv[2], 300.0):
        print(e)
