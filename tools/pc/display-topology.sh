#!/bin/sh
# Read-only IOAccelerator/IOFramebuffer topology dump (notes/DISPLAY-PAIRING.md Q5A). No GPU.
echo "===== ioreg -rc IOAccelerator ====="; ioreg -rc IOAccelerator -l -w0
echo "===== ioreg -rd1 -k MetalPluginName ====="; ioreg -rd1 -k MetalPluginName -w0
echo "===== ioreg -rc AMDRadeonX6000_AMDGraphicsAccelerator ====="; ioreg -rc AMDRadeonX6000_AMDGraphicsAccelerator -l -w0
echo "===== ioreg -rc IOFramebuffer (RDNA4FB + provider) ====="; ioreg -rc IOFramebuffer -l -w0
echo "===== grep AGXMetalA12 / MetalPluginName across the tree ====="; ioreg -l -w0 | grep -iE 'MetalPluginName|MetalPluginClassName|IOGLBundleName|AGXMetal'
