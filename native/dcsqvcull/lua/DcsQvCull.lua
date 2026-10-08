-- Loads the DcsQvCull native module (quad-view culling optimisation).
-- Install to: Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua
local ok, err = pcall(function()
  package.cpath = package.cpath .. ';' .. lfs.writedir() .. [[Scripts\DcsQvCull\?.dll]]
  require('DcsQvCull')
end)
if ok then
  log.write('DcsQvCull', log.INFO, 'native module loaded')
else
  log.write('DcsQvCull', log.ERROR, 'failed to load native module: ' .. tostring(err))
end
