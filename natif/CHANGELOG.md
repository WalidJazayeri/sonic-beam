# Sonic Beam natif — journal des versions

Le mode natif (dossier `natif/`) a ses propres versions : le numéro est dans `natif/VERSION` et le tag git est `natif-vX.Y.Z`, distinct des tags `vX.Y.Z` du mode superposé (`beamng/`). Les règles sont les mêmes :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

## natif-v0.1.0
- Premier prototype du mode natif, sans DLL : Sonic est un vrai objet de la scène de BeamNG (un StaticShape), éclairé par le jeu et caché par ce qui passe devant lui. Il ne projette pas encore d'ombre.
- Un convertisseur lit Sonic dans le Sonic Frontiers du joueur (modèle, squelette, animations de repos, de marche, de course, de boost et de chute, textures) et écrit un modèle COLLADA animé, ses matériaux et ses textures dans le dossier utilisateur de BeamNG (`mods/sonicnative_data`). Rien de Frontiers n'est dans le dépôt ni dans le mod.
- Le mod `sonicnative` place Sonic aux pieds du personnage à pied de BeamNG, le tourne dans le sens de sa course, cache le personnage du jeu et choisit l'animation et sa vitesse selon son allure. La physique de Sonic (extension `sonicnative_body`) est reprise du mode superposé.
- `natif/build.ps1` vérifie le Lua, construit le convertisseur et installe le mod dans le dossier utilisateur de dev (ou dans le dossier normal).
- Testé dans BeamNG sur Gridmap V2 : Sonic apparaît texturé, à pied dès le chargement de la carte, avec sa caméra de poursuite ; course (jusqu'à environ 90 km/h), boost (près de 300 km/h, course sur un mur), saut (environ 3 m) avec l'animation de chute, montée et descente de voiture ; il est caché par une voiture placée devant lui.
- Si la carte a été lancée sans le mode marche de BeamNG (`gameplay_walk`), le mod le charge lui-même pour mettre Sonic à pied ; pendant les 3 s où le nouveau corps se met en place, la caméra de Sonic est remise si BeamNG la réinitialise (ensuite, la touche C reste au choix du joueur).
