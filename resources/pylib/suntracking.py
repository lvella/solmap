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
            yield (v.az, v.alt)

        t += dt
