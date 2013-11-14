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

* `endpoints` &mdash; endpoints on which the proxy will listen for requests.
* `monitor-port` &mdash; the port to retrieve monitoring information. Just try the command `echo i | nc 127.0.0.1 20000`.
* `threads` &mdash; how many threads the proxy will run to handle requests. The proxy will run the same number of threads to communicate with Cocaine cloud.
* `locators` &mdash; just list of entrypoints to your cloud. Most time the proxy will use first locator in the list, but if it is dead then the proxy will use some other one.
* The proxy will write logs to the locator with `logging_prefix` source name.
* `service_pool` &mdash; how many connections the proxy will create to each application. I recomend to specify at least 3 connections to improve balancing and reduce impact of unstable network.
* The proxy will reconnect each connection every `reconnect_timeout` seconds (to rebalance requests to new nodes for examle).
* If an application doesn't reply on a request for `request_timeout` seconds then the proxy drops the request and replies with 504 code.
