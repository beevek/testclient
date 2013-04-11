#!/bin/sh

echo -n -e "Generating local file list..."
find /tmp/apache2-default/ -type f > ~/testclient/local.dat
echo -n -e "done\nGenerating md5 list..."
find /tmp/apache2-default/ -type f -exec md5sum -b {} \; | awk '{print $1}' > ~/testclient/md5.dat
echo -n -e "done\nGenerating URL list..."
find /tmp/apache2-default/ -type f | sed -e 's/\/tmp\/apache2-default\//http:\/\/mysite.com\//' > ~/testclient/urls.dat
echo "done"
