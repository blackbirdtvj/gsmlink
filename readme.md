Certs `https://curl.se/ca/cacert.pem`
gen bin `python gen_crt_bundle.py -i cacert.pem`
copy bundle to data/cert/.bin
test