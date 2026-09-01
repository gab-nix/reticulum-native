# Interface framing

The framing module provides bounded, allocation-free streaming codecs for byte
streams. Socket and serial interfaces supply decoder storage sized to their
maximum accepted Reticulum packet and can feed arbitrary chunks as they arrive.

## HDLC

HDLC frames use `0x7e` delimiters and `0x7d` byte escaping with the escaped byte
XORed by `0x20`. Bytes before the first delimiter are ignored. Empty frames are
keep-alives and do not invoke the callback.

## KISS

KISS frames use `FEND` (`0xc0`) delimiters and the standard `FESC TFEND` and
`FESC TFESC` substitutions. The decoder consumes the command byte and invokes
the callback only for KISS data commands; the port nibble is accepted but not
included in the delivered Reticulum packet.

## Recovery and ownership

Decoders never allocate and callback frame memory is valid only until the next
feed or reset. An invalid escape or a frame larger than the supplied storage is
discarded through the next delimiter, after which normal decoding resumes.
`malformed_frames` and `oversized_frames` are monotonic diagnostic counters.
Callback errors are returned immediately to the stream owner; framing errors
are counted and recovered in-stream.
