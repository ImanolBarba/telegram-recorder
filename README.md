# telegram-recorder
Telegram client that records messages in to a local sqlite3 database

Usage
--
> ⚠️ Please note that before usage, it is required to obtain an API ID and Hash from your telegram account. This does NOT give the recorder access to your account data, it merely gives it the ability to send API requests. This means you can reuse an API key for several accounts to be recorded, but if by chance your API key gets banned it'll break for all the accounts you use it for.

Since Telegram is multi-client, you can use this as a recorder alongside any other Telegram client.

While running, this client will download any files, photos (even profile/chat pictures), videos sent by anyone to the user, and write any messages sent to an SQLite database generated locally as `tgrec.db`. Downloaded files will be placed to folder named `download` by default.

When run for the first time, like ANY Telegram client, it will ask for your phone number and a notification for authorisation will be sent to the account associated to that phone number, which naturally you are expected to have access to. The authentication process is interactive in the terminal.

Configuration
--
This application needs a configuration file to be dropped in the same folder that the built binary is, in order to configure certain aspects of its behaviour.

Sample configuration:
```
# Authentication data
api_id = <YOUR_API_ID_HERE>
api_hash = "<YOUR_API_HASH_HERE>"

# Registration info
first_name = "John"
last_name = "Doe"

# Human behaviour parameters
# Average seconds to start reading messages, according to a normal distribution
read_msg_frequency_mean  = 600.0 
# Standard deviation of number of seconds to start reading messages, according to a normal distribution
read_msg_frequency_std_dev = 200.0
# Minimum wait enforced to start reading messages
read_msg_min_wait_sec = 10.0
# Text read speed in words per minute
text_read_speed_wpm = 250.0
# Photo "read" speed
photo_read_speed_sec = 5.0

# Other
download_folder = "download"
```

Most of the settings are self explanatory.

The ones related to the human behaviour statistical parameters refer to how tgrec reads messages. Telegram expects you to mark messages as read, it is stated explicitly in the API T&C that you must not implement a "ghost" mode that avoids marking any messages as read, so this is just to prevent being banned.

Besides this requirement, tgrec will try to "simulate" a normal human message reading pattern, which consists of:
1. Wait an arbitrary time (this is called the Inactive Period).
2. Wake up, start reading any unread messages from the queue (this is called the Active Period).
3. for each message read, wait an amount of time to read the next one, which depends on the message type and length. For pictures it's static, for video it's the video length, for text it's a certain amount of words per minute.
4. Repeat until everything is read
5. Go back to Inactive Period.

How to build
--
You will need:
- g++
- libconfig++-dev
- libspdlog-dev
- libsqlite3-dev
- libgtest-dev (Optional, only for unit tests)
- libssl-dev 
- cmake


```
(From repo root)
$ git submodule update --init
$ mdkir build
$ cmake ..
$ make -j 8
```

`make -j 8` is orientative, the number indicates how many CPUs it will use to compile. Keep in mind that compiling and linking `tdlib` is VERY SLOW.
