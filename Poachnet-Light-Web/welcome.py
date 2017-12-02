# Copyright 2015 IBM Corp. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import time
import calendar
import json
import traceback
import sys
from cloudant import Cloudant
from cloudant.adapters import Replay429Adapter
from werkzeug import secure_filename
from datetime import datetime
from dateutil import tz
from flask import Flask, jsonify, render_template, request, redirect, url_for
db_name = 'gps-db'
dateFormat = "%B %d, %Y - %H:%M:%S %p EST"
dir_path = os.path.dirname(os.path.realpath(__file__))
app = Flask(__name__)
app._static_folder = os.path.abspath("static/style.css")



def setup():
    client = None
    db = None
    if 'VCAP_SERVICES' in os.environ:
        vcap = json.loads(os.getenv('VCAP_SERVICES'))
        if 'cloudantNoSQLDB' in vcap:
            creds = vcap['cloudantNoSQLDB'][0]['credentials']
            user = creds['username']
            password = creds['password']
            host = 'https://' + creds['host']
            port = int(creds['port'])
            client = Cloudant(user, password, url=host, adapter=Replay429Adapter(retries=10), connect=True)
            client.create_database(db_name, throw_on_exists=False)
            db = client[db_name]
            return (client, db)





@app.route('/')
def welcome():
    return render_template('mainpage.html')





@app.route('/gpsa/<wordone>,<wordtwo>',  methods=['GET', 'POST'])
def addCoordRouteAnon(wordone, wordtwo):
    return addCoordRoute(wordone, wordtwo, '---', 'Anon', '---')





@app.route('/gps/<wordone>,<wordtwo>,<imei>,<dev>,<name>',  methods=['GET', 'POST'])
def addCoordRoute(wordone, wordtwo, imei, dev, name):
    try:
        print('[LOG] Recieved GPS coords.')
        client, db = setup()
        from_zone = tz.gettz('UTC')
        to_zone = tz.gettz('America/New_York')
        date = datetime.utcnow().replace(tzinfo=from_zone)
        date = date.astimezone(to_zone)
        datestr = date.strftime(dateFormat)
        data = {
                    'imei': imei,
                    'type': 'coord',
                    'dev': dev,
                    'recieved': datestr,
                    'lat': wordtwo,
                    'lng': wordone,
                    'name': name,
               }
        document = db.create_document(data)
        if document.exists():
            print('[LOG] Document created in DB.')
        else:
            print('[LOG] Document save error.')
        client.disconnect()
        return render_template('gps.html', coordinate1 = wordone, coordinate2 = wordtwo)
    except:
        print('[LOG] Error in /gps.')
        traceback.print_exc()




@app.route('/clear',  methods=['GET', 'POST'])
def clearDatabaseRoute():
    try:
        print('[LOG] Clearing database.')
        client, db = setup()
        for document in db:
            if (not 'type' in document) or document['type'] == 'coord':
                document.delete()
        client.disconnect()
        return redirect(url_for('showMapRoute'))
    except:
        print('[LOG] Error in /clear.')
        traceback.print_exc()





@app.route('/map')
def showMapRoute():
    try:
        print('[LOG] Creating map with saved points.')
        client, db = setup()
        maplist = []
        for document in db:
            if document['type'] == 'coord':
                maplist.append({'coord': '{lat: ' + document['lat'] + ', lng: ' + document['lng'] + '}',
                                'recTime1': document['recieved'],
                                'imei': document['imei'],
                                'dev': document['dev'],
                                'name': document['name'],
                                'n': 1, #Number of times this position has been reported.
                              })
        print(maplist)
        maplistTrimmed = []
        for c1 in maplist:   #Trim maplist to show compound points.
            flag = False
            for c2 in maplistTrimmed:
                if c1['coord'] == c2['coord'] and c1['imei'] == c2['imei'] and c1['dev'] == c2['dev'] and c1['name'] == c2['name']:
                    flag = True
                    c2['n'] += 1
                    addTime = datetime.strptime(c1['recTime1'], dateFormat)
                    time1 = datetime.strptime(c2['recTime1'], dateFormat)
                    if 'recTimeN' in c2:
                        timeN = datetime.strptime(c2['recTimeN'], dateFormat)
                    else:
                        timeN = None
                    if addTime < time1:
                         c2['recTime1'] = addTime.strftime(dateFormat)
                         c2['recTimeN'] = time1.strftime(dateFormat)
                    elif not timeN or addTime > timeN:
                        c2['recTime1'] = time1.strftime(dateFormat)
                        c2['recTimeN'] = addTime.strftime(dateFormat)
                    break
            if not flag:
                maplistTrimmed.append(c1)
        client.disconnect()
        return render_template('map.html', map = maplistTrimmed)
    except:
        print('[LOG] Error in /map.')
        traceback.print_exc()





port = os.getenv('PORT', '5001')
if __name__ == "__main__":
    app.run(host='0.0.0.0', port=int(port))
