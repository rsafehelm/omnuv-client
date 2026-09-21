# omnuv

The marketplace from a terminal.

```
omnuv --core https://core.example login   approve this terminal in a browser, once
omnuv machines                            what you have
omnuv launch gpu-2 --vcpus 4 --memory 16 --gpu "RTX 3090"
omnuv ssh gpu-2                           prints the command, address included
omnuv console gpu-2                       the browser console, which works when the network does not
omnuv events gpu-2                        what happened to it, in order
omnuv waiting                             requests waiting for capacity
omnuv rm gpu-2
```

Signing in is the device authorization flow: this prints a short code, you
approve it in a browser where you are already signed in, and it collects a
token. **No password is ever typed into a program you downloaded.** The token
lives in `~/.config/omnuv/cli.json`, readable only by you, and revoking it in
the console under Network stops it working on the next request.

`omnuv ssh` prints the command rather than running it. That way it composes with
whatever you were going to do, and nothing surprises you by opening a shell.

It holds no marketplace logic and never will: it asks Core what exists and
prints it. Every decision about placement, price and provider stays on the other
end of the wire.
