#!/usr/bin/env python3

import ephem
import datetime
import functools
import math

# Inverse golden ratio
phi = 2.0 / (1.0 + 5.0**0.5)

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

def sun_pos(obs, time):
    obs.date = time.strftime('%Y/%m/%d %H:%M:%S')
    v = ephem.Sun(obs)

    return v.az, v.alt

def altitude(reference, obs, x):
    time = reference + datetime.timedelta(seconds=x)
    return sun_pos(obs, time)[1]

def daytimes_over_year(obs):
    # Timezone is used to identify which calendar day and
    # month is identified by the solar database, given
    # a date in UTC. This is only important in places near
    # the international date line.
    timezone = float(obs.longitude) / 15.0

    # We start from a reference date we know to be near the
    # lowest sun position on 2017-01-01 at the given position.
    lowest = (
        datetime.datetime(2018, 1, 1, 0, 0, 0)
        + datetime.timedelta(hours=timezone)
    )

    # Find the time of lowest sun position for the date:
    fn = functools.partial(altitude, lowest, obs)
    delta, l_alt = int_minimize(fn, -6*3600, 6*3600)
    lowest += datetime.timedelta(seconds=delta)

    date = datetime.date(2018, 1, 1)
    for i in range(365):

        # Find sun's peak:
        highest = lowest + datetime.timedelta(hours=12)
        fn = lambda x: -altitude(highest, obs, x)
        delta, h_alt = int_minimize(fn, -6*3600, 6*3600)
        h_alt = -h_alt
        highest += datetime.timedelta(seconds=delta)

        if l_alt >= 0:
            day_start = lowest
        elif h_alt > 0:
            # Find sunrise:
            half_delta = (highest - lowest)*0.5
            sunrise = lowest + half_delta
            fn = functools.partial(altitude, sunrise, obs)
            half_delta = half_delta.total_seconds()
            delta = int_root_find(fn, -half_delta, half_delta)
            sunrise += datetime.timedelta(seconds=delta)

            day_start = sunrise
        else:
            day_start = None

        # Find next lowest position:
        lowest = highest + datetime.timedelta(hours=12)
        fn = functools.partial(altitude, lowest, obs)
        delta, l_alt = int_minimize(fn, -6*3600, 6*3600)
        lowest += datetime.timedelta(seconds=delta)

        if h_alt <= 0 or l_alt >= 0:
            day_finish = lowest
        else:
            # Find sundown:
            half_delta = (lowest - highest)*0.5
            sundown = highest + half_delta
            fn = lambda x: -altitude(sundown, obs, x)
            half_delta = half_delta.total_seconds()
            delta = int_root_find(fn, -half_delta, half_delta)
            sundown += datetime.timedelta(seconds=delta)

            day_finish = sundown

        if day_start:
            yield (day_start, day_end)

def quantize_year(latitude, longitude, max_dt):
    obs = ephem.Observer()
    obs.lat = str(latitude)
    obs.lon = str(longitude)
    obs.elevation = 0

    for day in daytimes_over_year(obs):
        delta = (day_end - day_start).total_seconds()
        n = max(2, int(math.ceil(delta / max_dt)))
        dt = float(delta) / n

        # Start day integration, using trapezoidal rule
        # https://en.wikipedia.org/wiki/Trapezoidal_rule
        az, alt = sun_pos(obs, day_start)

        # Coefficient for the first term of trapezoidal rule: dt/2
        yield (dt*0.5, az, alt)

        # Middle terms for trapezoidal rule:
        for i in range(1, n):
            az, alt = sun_pos(
                obs,
                day_start + datetime.timedelta(seconds=i*dt)
            )
            yield (dt, az, alt)

        print('###', (day_end - day_start + datetime.timedelta(seconds=i*dt)).total_seconds(), dt)

        # Last term for trapezoidal rule (it uses n+1 points for n chunks):
        az, alt = sun_pos(obs, day_end)
        yield (dt*0.5, az, alt)


if __name__ == '__main__':
    import sys
    daytimes_over_year(sys.argv[1], sys.argv[2])
