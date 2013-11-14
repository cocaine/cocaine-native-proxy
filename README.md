Cocaine Native Proxy
====================

This proxy provides HTTP interface to a Cocaine cloud.

It selects an application and an event based on headers `X-Cocaine-Service` and `X-Cocaine-Event` or, if some of these headers are ommited, then based on URL (using format `http://host/app/event...`).

Building
========

Just run:
<pre>
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ./
make
</pre>

You need [Cocaine Native Framework](https://github.com/cocaine/cocaine-framework-native), [Cocaine](https://github.com/cocaine/cocaine-core) development files and [Swarm](https://github.com/reverbrain/swarm) to build the proxy.

Usage
=====

Run the proxy as follows:
<pre>
cocaine-native-proxy -c &lt;config&gt;
</pre>

Or if you use init-script from `debian/` folder then just place your config to `/etc/cocaine-native-proxy/` and restart the proxy with command `sudo service cocaine-native-proxy restart`.
Your config must have extension `*.conf`.

Example config
==============

```JSON
{
    "endpoints": [
        "0.0.0.0:8080"
    ],
    "daemon": {
        "monitor-port": 20000
    },
    "backlog": 2048,
    "threads": 2,
    "application": {
        "locators": ["127.0.0.1:10053"],
        "logging_prefix": "cocaine-proxy-01",
        "service_pool": 10,
        "reconnect_timeout": 180,
        "request_timeout": 5
    }
}
```
