# Network Connections for Xfce

**Network Connections** (`xfce-net-plugin`) is an Xfce panel plugin for managing Wi-Fi and Ethernet connections through NetworkManager. It provides a panel-based alternative to `nm-applet` for common connection tasks and can handle NetworkManager password requests when enabled.

[Leer en español](#español)

## Features

- Connect to, disconnect from, and manage Wi-Fi networks through the panel.
- Support for multiple Wi-Fi adapters, hidden networks, and per-adapter Wi-Fi controls.
- WPA2/WPA3 indicators and 2.4 / 5 / 6 GHz band information.
- Ethernet connections, including USB tethering where exposed by NetworkManager.
- NetworkManager connection profiles, including VPN and WireGuard profiles.
- Search available networks using the search button, `Ctrl+F`, or by typing.
- Context actions for connection details, automatic connection, sharing and forgetting saved networks.
- Connection information including IP addresses, gateway, DNS, IPv6, channel and BSSID.
- Optional QR codes for sharing a connected Wi-Fi network (requires libqrencode at build time).
- Optional desktop notifications (requires libnotify at build time).
- Captive-portal notice when NetworkManager connectivity checking is enabled.
- Configurable panel button, icon appearance, popup size and popup style.
- Optional password agent for NetworkManager and optional restoration of disabled adapters.
- Keyboard navigation and translation catalogs for 11 languages.

The plugin communicates with NetworkManager over D-Bus, without a direct dependency on `libnm`. **NetworkManager remains required**; the plugin does not replace the NetworkManager service.

## Requirements

- Xfce panel 4.18 or later, GTK 3.24 or later, and Xfconf 4.18 or later.
- NetworkManager running and managing the relevant network interfaces.
- A working D-Bus session and the appropriate system permissions for network management.

The project has been tested in X11 and Wayland sessions (including labwc and xfwl4). Actual behavior can depend on the distribution, panel and compositor versions.

## Installation

### Published packages

See [GitHub Releases](https://github.com/Tantin1/xfce-net-plugin/releases) for available packages and their supported distributions. Development builds use the version declared in [`src/VERSION`](src/VERSION), which may differ from the latest release.

### Build from source

Install the required development tools and libraries:

**Arch Linux and derivatives:**

```bash
sudo pacman -S --needed base-devel git cmake pkgconf gettext gtk3 xfce4-panel xfconf
# Optional features and Meson builds:
sudo pacman -S --needed qrencode libnotify meson ninja
```

**Debian 12 / Debian 13:**

```bash
sudo apt install build-essential git cmake pkg-config gettext \
  libgtk-3-dev libxfce4panel-2.0-dev libxfconf-0-dev
# Optional features and Meson builds:
sudo apt install libqrencode-dev libnotify-dev meson ninja-build
```

Clone the repository:

```bash
git clone https://github.com/Tantin1/xfce-net-plugin.git
cd xfce-net-plugin
```

**Using CMake:**

```bash
cmake -S src -B build
cmake --build build
sudo cmake --install build
xfce4-panel -r
```

**Or, using Meson:**

```bash
meson setup build src --prefix=/usr
meson compile -C build
sudo meson install -C build
xfce4-panel -r
```

Then open **Panel Preferences → Items → Add** and select **Network Connections**.

## Configuration and notes

- **Settings:** each panel instance stores its own settings in Xfconf, in the `xfce4-panel` channel under its instance-specific `/plugins/plugin-N` path. Earlier `~/.config/xfce4/panel/xfce-net-plugin.ini` settings are imported once for migration; that file is no longer the active settings store.
- **Using `nm-applet` simultaneously:** if both programs act as NetworkManager password agents, password prompts may compete. Disable **Respond to password requests** in this plugin's Properties → Network, or disable the `nm-applet` autostart entry.
- **Captive portals:** the notice relies on NetworkManager connectivity checking. On Debian, the `network-manager-config-connectivity-debian` package can enable the distribution's connectivity-check configuration.
- **Optional features:** QR sharing and desktop notifications are available only when their respective libraries were present during compilation.

## License and author

Copyright © 2026 [Tantin1](https://github.com/Tantin1) ([contact](mailto:juanperez33840@gmail.com)).

The original plugin source code is licensed under the **GNU General Public License, version 2 or (at your option) any later version** (`GPL-2.0-or-later`). See [LICENSE](LICENSE).

**Third-party icon:** `src/icons/nm-secure-lock.svg` comes from the [Papirus icon theme](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme), maintained by the Papirus Development Team and contributors, and is redistributed under **GPL-3.0-only**. See [the GPLv3 license text](LICENSES/GPL-3.0-only.txt). Distributions containing this icon together with the plugin must comply with GPLv3 for the combined work; the licensing of the original plugin source files remains GPL-2.0-or-later.

---

## Español

**Network Connections** (`xfce-net-plugin`) es un complemento para el panel de Xfce que permite gestionar conexiones Wi-Fi y Ethernet mediante NetworkManager. Es una alternativa a `nm-applet` para las tareas habituales de conexión y, si se habilita la opción, puede responder a las solicitudes de contraseña de NetworkManager.

## Características

- Conectar, desconectar y administrar redes Wi-Fi desde el panel.
- Varios adaptadores Wi-Fi, redes ocultas e interruptores independientes por adaptador.
- Indicadores WPA2/WPA3 y bandas de 2,4 / 5 / 6 GHz.
- Conexiones Ethernet, incluida la conexión compartida por USB cuando NetworkManager la detecta.
- Perfiles de conexión de NetworkManager, incluidas VPN y WireGuard.
- Búsqueda de redes mediante el botón, `Ctrl+F` o escribiendo directamente.
- Acciones contextuales para detalles, conexión automática, compartir y olvidar redes guardadas.
- Información de IP, puerta de enlace, DNS, IPv6, canal y BSSID.
- Códigos QR opcionales para compartir redes Wi-Fi (requiere libqrencode al compilar).
- Notificaciones opcionales (requiere libnotify al compilar).
- Avisos de portal cautivo si NetworkManager tiene habilitada la comprobación de conectividad.
- Personalización del botón, icono, tamaño y estilo de la ventana emergente.
- Agente de contraseñas opcional y opción de recordar los adaptadores desactivados.
- Navegación con teclado y catálogos de traducción para 11 idiomas.

El complemento se comunica con NetworkManager mediante D-Bus, sin depender directamente de `libnm`. **NetworkManager sigue siendo necesario**: el complemento no sustituye su servicio.

## Requisitos

- Panel de Xfce 4.18 o superior, GTK 3.24 o superior y Xfconf 4.18 o superior.
- NetworkManager ejecutándose y gestionando las interfaces de red correspondientes.
- Sesión D-Bus y permisos del sistema adecuados para administrar las conexiones.

El proyecto se probó en sesiones X11 y Wayland (incluidos labwc y xfwl4). El resultado puede variar según las versiones de la distribución, el panel y el compositor.

## Instalación

### Paquetes publicados

Consultá [GitHub Releases](https://github.com/Tantin1/xfce-net-plugin/releases) para ver qué paquetes hay disponibles y para qué distribuciones. Las compilaciones de desarrollo utilizan la versión declarada en [`src/VERSION`](src/VERSION), que puede ser distinta de la última versión publicada.

### Compilar desde el código fuente

**Arch Linux y derivadas:**

```bash
sudo pacman -S --needed base-devel git cmake pkgconf gettext gtk3 xfce4-panel xfconf
# Funciones opcionales y compilación con Meson:
sudo pacman -S --needed qrencode libnotify meson ninja
```

**Debian 12 / Debian 13:**

```bash
sudo apt install build-essential git cmake pkg-config gettext \
  libgtk-3-dev libxfce4panel-2.0-dev libxfconf-0-dev
# Funciones opcionales y compilación con Meson:
sudo apt install libqrencode-dev libnotify-dev meson ninja-build
```

Cloná el repositorio:

```bash
git clone https://github.com/Tantin1/xfce-net-plugin.git
cd xfce-net-plugin
```

**Con CMake:**

```bash
cmake -S src -B build
cmake --build build
sudo cmake --install build
xfce4-panel -r
```

**O con Meson:**

```bash
meson setup build src --prefix=/usr
meson compile -C build
sudo meson install -C build
xfce4-panel -r
```

Después, abrí **Preferencias del panel → Elementos → Añadir** y seleccioná **Conexiones de red** (o **Network Connections**, según el idioma del panel).

## Configuración y notas

- **Configuración:** cada instancia guarda sus ajustes en Xfconf, en el canal `xfce4-panel`, bajo su ruta `/plugins/plugin-N`. La configuración antigua de `~/.config/xfce4/panel/xfce-net-plugin.ini` se importa una sola vez; ya no es el almacenamiento activo.
- **Con `nm-applet` en ejecución:** ambos pueden intentar responder a las solicitudes de contraseña. Desactivá **Responder a las solicitudes de contraseña** en Propiedades → Red o quitá `nm-applet` del inicio automático.
- **Portales cautivos:** el aviso necesita la comprobación de conectividad de NetworkManager. En Debian, el paquete `network-manager-config-connectivity-debian` puede habilitar la configuración de comprobación de conectividad de la distribución.
- **Funciones opcionales:** los códigos QR y las notificaciones requieren que sus bibliotecas estuvieran presentes al compilar.

## Licencia y autor

Copyright © 2026 [Tantin1](https://github.com/Tantin1) ([contacto](mailto:juanperez33840@gmail.com)).

El código original del complemento se distribuye bajo la **GNU General Public License, versión 2 o (a tu elección) cualquier versión posterior** (`GPL-2.0-or-later`). Consultá [LICENSE](LICENSE).

**Icono de terceros:** `src/icons/nm-secure-lock.svg` procede del [tema de iconos Papirus](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme), mantenido por Papirus Development Team y sus colaboradores, y se redistribuye bajo **GPL-3.0-only**. Consultá el [texto de la licencia GPLv3](LICENSES/GPL-3.0-only.txt). Las distribuciones que combinan este icono con el complemento deben respetar la GPLv3 para el conjunto; los archivos de código originales conservan su licencia GPL-2.0-or-later.
