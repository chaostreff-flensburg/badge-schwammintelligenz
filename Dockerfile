# Statische Web-Steuerung (web/) für Coolify oder jeden anderen Docker-Host.
FROM nginx:alpine
COPY web/ /usr/share/nginx/html/
