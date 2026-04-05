


ssh root@74.208.158.226 -t tmux attach || tmux
XgG952uw
ssh denis@rephrasely.com.ar -t tmux attach || tmux
2026!ionos
GithubRepo : https://github.com/bueltan/rephrasely
https://github.com/bueltan/rephrasely.git



https://chatgpt.com/share/694f47fd-4f34-8003-86cf-9118ae4b227b

tmux            # start a new session
Ctrl+b, c       # create a new window
Ctrl+b, n/p     # next/previous window
Ctrl+b, %       # split vertically
Ctrl+b, "       # split horizontally

Ctrl+b, d       # detach (session keeps running)
tmux attach     # reattach to session
-------------------------------------------
Start+enable the instance for your user:
sudo systemctl enable --now "flaskapp@d_gimenez"
Check status/logs:
sudo systemctl status "flaskapp@d_gimenez" --no-pager
sudo journalctl -u "flaskapp@d_gimenez" -e --no-pager
---------------------------------------------------
Test nginx :Step 3) Test NGINX config and reload
sudo nginx -t
sudo systemctl reload nginx

cerbot :
sudo certbot --nginx -d rephrasely.com.ar  -d www.rephrasely.com.ar
