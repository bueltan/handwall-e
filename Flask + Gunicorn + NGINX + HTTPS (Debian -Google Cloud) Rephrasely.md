

````markdown
# 🚀 Flask + Gunicorn + NGINX + HTTPS (Debian / Google Cloud)

## 1. Install base packages
```bash
sudo apt update
sudo apt install -y nginx python3 python3-venv python3-pip
````

---

## 2. Create Flask app

```bash
mkdir -p ~/apps
cd ~/apps

python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install flask gunicorn
git clone https://github.com/bueltan/rephrasely.git
cd rephrasely
poetry lock
poetry install


Test app:

cat > app.py << 'PY'
from flask import Flask
app = Flask(__name__)

@app.get("/")
def index():
    return "Hello from Flask behind NGINX + Gunicorn + HTTPS!"
PY
```

Test locally:

```bash
gunicorn -w 2 -b 127.0.0.1:8000 app:app
```

---

## 3. Systemd service (templated)

Create **`/etc/systemd/system/rephrasely@.service`**:

```ini
[Unit]
Description=Gunicorn for Rephrasely (%i)
After=network.target

[Service]
Type=simple
User=%i
Group=%i
EnvironmentFile=/home/%i/apps/rephrasely/.env
WorkingDirectory=/home/%i/apps/rephrasely/rephrasely/src
Environment="PATH=/home/%i/apps/rephrasely/.venv/bin"
ExecStart=/home/%i/apps/rephrasely/.venv/bin/gunicorn \
  --workers 2 \
  --bind 127.0.0.1:8000 \
  --access-logfile - \
  --error-logfile - \
  app:app
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target

```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now "rephrasely@denis"
sudo systemctl status "rephrasely@denis"

`sudo systemctl stop "rephrasely@denis"`

# Logs :
sudo journalctl -u rephrasely@denis -n 80 --no-pager
```

---

## 4. NGINX reverse proxy (HTTP)

`sudo nano /etc/nginx/sites-available/rephrasely.com.ar`

```nginx
server {
    listen 80;
    server_name rephrasely.com.ar www.rephrasely.com.ar;

    location / { 
        proxy_pass http://127.0.0.1:8000;

        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }

    location /nginx-health {
        return 200 "ok\n";
        add_header Content-Type text/plain;
    }
}
```

Enable:

```bash
sudo ln -s /etc/nginx/sites-available/rephrasely.com.ar` /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t
sudo systemctl reload nginx
```

---

## 5. Configure DNS in DonWeb

- **A record** → `rephrasely.com.ar` → `34.135.175.41`
    
- **A record** → `www.rephrasely.com.ar` → `34.135.175.41`
    
    

Check propagation:

```bash
dig rephrasely.com.ar +short
dig www.rephrasely.com.ar +short
```

---

## 6. HTTPS with Certbot

Install Certbot:

```bash
sudo apt install -y certbot python3-certbot-nginx
```

Request certificates:

```bash
sudo certbot --nginx -d rephrasely.com.ar -d www.rephrasely.com.ar
```

Verify auto-renewal:

```bash
sudo certbot renew --dry-run
```

---

## 7. Verify and harden NGINX

Check site:

```bash
curl -I https://rephrasely.com.ar
```

Optional security header in HTTPS block:

```nginx
add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
```

Reload:

```bash
sudo nginx -t && sudo systemctl reload nginx
```

---

## ✅ Final Notes

- Gunicorn runs under systemd: `flaskapp@d_gimenez`.
    
- NGINX proxies HTTP → Gunicorn (127.0.0.1:8000).
    
- HTTPS handled by Certbot (Let’s Encrypt).
    
- DNS points correctly to Google Cloud VM.
    
- Certificates auto-renew.
