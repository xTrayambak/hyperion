{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
	nativeBuildInputs = with pkgs.buildPackages; [
		clang
		gnumake
		pkg-config
		bear
		
		wayland
		wayland-protocols
		wayland-scanner.dev
		wlroots
		libdrm.dev
		libxkbcommon
		pango.dev
		pixman
		systemd.dev
	];
}
