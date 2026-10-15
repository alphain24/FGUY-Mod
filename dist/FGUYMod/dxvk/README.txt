DXVK 3.1.1 (https://github.com/doitsujin/dxvk, by Philip Rebohle and contributors, zlib license - see LICENSE.txt):
the 32-bit d3d9.dll of the official release dxvk-3.1.1.tar.gz, renamed dxvk_d3d9.dll.
The mod loads it by default (FGUYMod.ini [Display] Renderer=dxvk): the game's Direct3D 9 then runs on Vulkan.
Renderer=d3d9 turns it off; a PC without Vulkan 1.3 gets plain Direct3D 9 by itself.
