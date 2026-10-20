# Sonic Beam — journal des versions

Versions au format `vMAJEUR.MINEUR.PATCH` (le numéro est dans `beamng/VERSION`, le tag git est `vX.Y.Z`) :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

## v0.1.4
- Menu (touche O) : bouton « Unstuck: back to the last safe spot » pour remettre Sonic debout quand il est coincé (rien d'automatique).
- Après une réinitialisation du corps de marche, la traînée réduite de Sonic est réappliquée.
- La version vient du fichier `VERSION` (build et journal du rendu).

## v0.1.3
- Rendu de Sonic par `version.dll` (Bin64) et liaison UDP locale avec le script Lua ; Direct3D 11 et 12.
- Compteur de vitesse, boost sans plafond avec limite réglable, menu en jeu (touche O).
- Animations Frontiers dont la cadence suit la vitesse et plafonne en douceur.
- Haute vitesse : glisse, sans traînée d'air, accroche au sol ; saut propre à Sonic.
- Sonic ne prend la main qu'en mode Libre (pas dans le tutoriel, la carrière, les scénarios ni les missions).

## v0.1.0 – v0.1.1
- Premières versions envoyées sur Melty (liaison FFI, abandonnée : BeamNG l'interdit aux mods).
