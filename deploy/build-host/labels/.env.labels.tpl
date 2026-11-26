# op inject template: the corpus deploy key (write access to Jdubz/blinky-corpus only), as base64 of
# the OpenSSH private key (an env file can't hold multiple lines). deploy.sh renders it into a
# transient file for one `docker compose up`; the service writes it to a 0600 file on its tmpfs.
REVIEWER_DEPLOY_KEY_B64=op://Development/blinky labels corpus deploy key/private_key_b64
