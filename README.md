<p align="center"><img src="https://robotraconteurpublicfiles.s3.amazonaws.com/RRheader2.jpg"></p>

# Robot Raconteur Core QUIC Transport

This repository contains a **highly experimental** implementation of a QUIC transport for the [Robot Raconteur core library](https://github.com/robotraconteur/robotraconteur). The contents of this library will eventually be merged into the primary repository.

QUIC has been selected by the IETF for use as the transport for HTTP/3, replacing TCP, which was used for HTTP/1 through HTTP/2. QUIC was originally developed by Google for use with Chrome. Using the initial work by Google, the IETF is developing a new (incompatible) version of the QUIC protocol as an internet standard. The status of this new version can be found [here](https://datatracker.ietf.org/doc/draft-ietf-quic-transport/). The IETF QUIC protocol is still under active development and has not been finalized.

QUIC offers several advantages over TCP, as discussed in the IETF documents. The two most significant advantages for use with Robot Raconteur are:

* Stream multiplexing, allowing for multiple messages to be sent simultaneously without extra logic
* No head-of-line blocking between streams

Head-of-line blocking is the most significant improvement provided by QUIC. With TCP, a single lost or corrupted packet will block all data transmission until it has been corrected. This is disastrous for real-time scenarios where a single lost message may not be a big deal, but delaying all messages is. With TCP, the single lost messages will stop all transmission. With QUIC, each message can have its own stream. This concept is called "streams-as-messages" in QUIC, since opening and closing streams is very inexpensive. Each stream is completely independent for error checking, so even if one or more streams have a problem, the rest will still continue to be received normally.

QUIC has great potential as an industrial automation protocol, and since it will be used for HTTP/3, will be widely supported. There are even plans for [web browser and WebRTC support](https://w3c.github.io/webrtc-quic/), allowing for browser-to-device interoperability. (This is currently available in Robot Raconteur using WebSockets, with all the disadvantages of TCP.)

[picoquic](https://github.com/private-octopus/picoquic) is used for the QUIC protocol implementation.

This repository is for demonstration only. It should not be deployed to a production environment.

Currently TLS certificate loading and verifying are not supported. The test certificates provided by picoquic are used to allow the transport to function.

License: Apache 2.0
