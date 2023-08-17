Create config guides for each board

recommended fps and resolution. Zero W is the trickiest. Keeps stalling on me.

# Raspberry Pi board setup

1. Use rpi-imager to install Pi OS Lite to an SDCard
1. Boot your pi. Set the hostname, enable the legacy camera interface, timezone, keyboard and locale
1. apt update && apt upgrade
1. apt install git
1. Clone the pimera repo
1. cd mmal-version
1. Run make target for your board: make four, make three, make zero
1. Make ~/h264 folder to store videos
1. Adjust mmal-version/settings.ini as desired
1. Adjust cpu-metrics/cpu-metrics.service for your StatsD / InfluxDB server. MORE WORK NEEDED HERE
1. Copy cpu-metrics/cpu-metrics.service
1. Adjust mmal-version/pimera.service if necessary and copy to /etc/systemd/system
1. systemctl daemon-reload
1. Use systemctl enable on cpu-metrics and pimera if you want them to start on boot
1. Run systemctl start cpu-metrics to start CPU usage and temperature collection now
1. Run systemctl start pimera to start PiMera now
1. Open http://IP_OF_PI:8080 to view the live stream and select the detection region


# Storage and processing server setup

I use Debian.

TODO: adapt video-browser/api.py to work without Caddy

1. Install mysql/mariadb, python3 and venv, the caddy webserver
1. Clone the pimera repository
