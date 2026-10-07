# BT-Guard

BT-Guard is an app for Pebble Watches that helps to maintain the connection with your phone, so you don't fail to realise that it's not close by and running.  This app also helps you find your phone again.

Downloads and User information about the app are on the store page at https://apps.repebble.com/61b1a39099e740eabd674088 

# The App

The files for the app itself include:

    package.json
    src/
      c/           - most of the app
      pkjs/        - bits that run on the phone
    worker_src/    - wakes the app up periodically when disconnected, or when you reconnect
    resources/     - images

# Release process

This app is published in the app store at https://apps.repebble.com/61b1a39099e740eabd674088

Besides the app itself, this repository includes tools I use for publishing there and files associated with that process.

    store/
    CHANGELOG.md
    tools/prepare_release.py

# Design

Design resources like svg files for producing visual assets used in the app are kept here.

    design/