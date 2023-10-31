from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
import logging
import asyncio
import socket
from aiocoap import *

import time

# put your board's IP address here
ESP32_IP = "192.168.100.150"

# un comment the type of test you want to execute
TEST = "GET"
#TEST = "PUT"
#TEST = "DELETE"

URI_shoelace = "shoe/shoelace"
PAYLOAD_shoelace = b"tie"
#PAYLOAD = b"untie"

URI_ledcolor = "shoe/ledcolor"
PAYLOAD_ledcolor = b"123456"

URI_steps = "shoe/steps"

URI_size = "shoe/size"

URI_name = "shoe/name"
PAYLOAD_name = b"Judith"

logging.basicConfig(level=logging.INFO)

async def get(ip, uri):
    protocol = await Context.create_client_context()
    request = Message(code = GET, uri = 'coap://' + ip + '/' +  uri)
    try:
        response = await protocol.request(request).response
    except Exception as e:
        print('Failed to fetch resource:')
        print(e)
    else:
        print('Result: %s\n%r'%(response.code, response.payload))

async def put(ip, uri, payload):
    context = await Context.create_client_context()
    await asyncio.sleep(2)
    request = Message(code = PUT, payload = payload, uri = 'coap://' + ip +'/' + uri)
    response = await context.request(request).response
    print('Result: %s\n%r'%(response.code, response.payload))

async def delete(ip, uri):
    context = await Context.create_client_context()
    await asyncio.sleep(2)
    request = Message(code = DELETE, uri = 'coap://' + ip +'/' + uri)
    response = await context.request(request).response
    print('Result: %s\n%r'%(response.code, response.payload))

if __name__ == "__main__":
  asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

  zeroconf = Zeroconf()
  info = zeroconf.get_service_info("_coap._udp.local.", "shoe_control._coap._udp.local.")

  #if info:
    #print("Device found: {}".format(info))
    #ESP32_IP = socket.inet_ntoa(info.addresses[0])
    #print("IP Address: " + ESP32_IP)
  #else:
    #print("Device not found!")
    #exit(1)

  if(1):
    print("*** GET Shoelace ***")
    asyncio.run(get(ESP32_IP, URI_shoelace))
    print("*** PUT shoelace***")
    asyncio.run(put(ESP32_IP, URI_shoelace, PAYLOAD_shoelace))
    print("*** GET Shoelace ***")
    asyncio.run(get(ESP32_IP, URI_shoelace))
  
  #Test LedColor
    print("*** GET Ledcolor ***")
    asyncio.run(get(ESP32_IP, URI_ledcolor))
    print("*** PUT ledcolor***")
    asyncio.run(put(ESP32_IP, URI_ledcolor, PAYLOAD_ledcolor))
    print("*** GET ledcolor ***")
    asyncio.run(get(ESP32_IP, URI_ledcolor))
    print("*** DELETE ledcolor ***")
    asyncio.run(delete(ESP32_IP, URI_ledcolor))
    print("*** GET ledcolor***")
    asyncio.run(get(ESP32_IP, URI_ledcolor))

  #Test Steps
    print("*** GET steps ***")
    asyncio.run(get(ESP32_IP, URI_steps))
    time.sleep(10)
    print("*** GET steps ***")
    asyncio.run(get(ESP32_IP, URI_steps))
  
  #Test Size
    print("*** GET size ***")
    asyncio.run(get(ESP32_IP, URI_size))
  
  #Test name
    print("*** GET name ***")
    asyncio.run(get(ESP32_IP, URI_name))
    print("*** PUT name***")
    asyncio.run(put(ESP32_IP, URI_name, PAYLOAD_name))
    print("*** GET name ***")
    asyncio.run(get(ESP32_IP, URI_name))
    print("*** DELETE name ***")
    asyncio.run(delete(ESP32_IP, URI_name))
    print("*** GET name ***")
    asyncio.run(get(ESP32_IP, URI_name))

