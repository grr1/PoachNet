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
client = None
db = None
#datere = "%Y-%m-%d_%H-%M-%S"
dateFormat = "%B %d, %Y - %H:%M:%S %p EST"
dir_path = os.path.dirname(os.path.realpath(__file__))
app = Flask(__name__)



def setup():
    global client
    global db
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
            print('[LOG] Setup complete.')




class Testclass(object):
    fullpath = ""
    name = ""
    def __init__(self, fullpath, name):
        self.fullpath = fullpath
        self.name = name


@app.route('/')
def welcome():
    print(app.instance_path)
    print(app.root_path)
    print(dir_path)
    directories = []
    if(os.path.exists(dir_path + "/static/images/")):
        for filename in os.listdir(dir_path + "/static/images/"):
            print(filename)
            meme = Testclass(filename, filename)
            directories.append(meme)
            print(dir_path + '/static/images/' + filename)
    return render_template('mainpage.html', dirs=directories)


@app.route('/pictures/<path:directory>')
def picture(directory):
    directories = []
    for filename in os.listdir(dir_path + '/static/images/' + directory):
        print(filename)
        if (filename.endswith('.jpg') or filename.endswith('.png') or filename.endswith('.jpeg')):
            return display_images(directory)
        meme = Testclass(directory + '/' + filename, filename)
        directories.append(meme)
    return render_template('mainpage.html', dirs=directories)


def display_images(directory):
    images = []
    for filename in os.listdir(dir_path + '/static/images/' + directory):
        images.append(directory + '/' + filename)
    return render_template('index.html', imagelist=images)


@app.route('/iotpost')
def iotpost():
    return render_template('iotpost.html')


@app.route('/uploader', methods=['GET', 'POST'])
def upload_file():
    if request.method == 'POST':
        f = request.files['file']
        #d = time.strptime(f.filename, datere)
        t = f.filename
        print(f.filename)
        #print(t)
        #print(t[t.index('_',t.index('_')+1):])
        #d = time.strptime(f.filename, datere + t[t.index('_',t.index('_')+1):])
        #print(str(d.tm_year))
        #print('milestone1')
        #filepath = os.path.join(
         #   dir_path + "/static/images/" + str(d.tm_year) + '/' + calendar.month_name[d.tm_mon] + '/' + str(
        #        d.tm_mday) + '/', secure_filename(f.filename))
        #new_dir_path = dir_path + "/static/images/" + str(d.tm_year) + '/' + calendar.month_name[d.tm_mon] + '/' + str(
        #        d.tm_mday) + '/'
        new_dir_path = dir_path + "/static/images/dir/"
        #print(filepath)
        #print('milestone2')
        if not os.path.exists(new_dir_path):
            os.makedirs(new_dir_path)
        #os.makedirs(os.path.dirname(filepath), exist_ok=True)
        #print('milestone3')
        f.save(new_dir_path + secure_filename(f.filename))
        return 'file uploaded successfully'





@app.route('/gpsa/<wordone>,<wordtwo>',  methods=['GET', 'POST'])
def addCoordRouteAnon(wordone, wordtwo):
    return addCoordRoute(wordone, wordtwo, '---', 'Anon', '---')





@app.route('/gps/<wordone>,<wordtwo>,<imei>,<dev>,<name>',  methods=['GET', 'POST'])
def addCoordRoute(wordone, wordtwo, imei, dev, name):
    try:
        print('[LOG] Recieved GPS coords.')
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
                    'lat': wordone,
                    'lng': wordtwo,
                    'name': name,
               }
        document = db.create_document(data)
        if document.exists():
            print('[LOG] Document created in DB.')
        else:
            print('[LOG] Document save error.')
        return render_template('gps.html', coordinate1 = wordone, coordinate2 = wordtwo)
    except:
        print('[LOG] Error in /gps.')
        traceback.print_exc()




@app.route('/clear',  methods=['GET', 'POST'])
def clearDatabaseRoute():
    try:
        print('[LOG] Clearing database.')
        for document in db:
            if (not 'type' in document) or document['type'] == 'coord':
                document.delete()
        return redirect(url_for('showMapRoute'))
    except:
        print('[LOG] Error in /clear.')
        traceback.print_exc()





@app.route('/map')
def showMapRoute():
    try:
        print('[LOG] Creating map with saved points.')
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
        flag = False
        for c1 in maplist:
            for c2 in maplistTrimmed:
                if c1['coord'] == c2['coord'] and c1['imei'] == c2['imei']:
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
        return render_template('map.html', map = maplistTrimmed)
    except:
        print('[LOG] Error in /map.')
        traceback.print_exc()





port = os.getenv('PORT', '5001')
if __name__ == "__main__":
    setup()
    app.run(host='0.0.0.0', port=int(port))
