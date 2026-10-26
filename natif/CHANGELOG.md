# Sonic Beam natif — journal des versions

Le mode natif (dossier `natif/`) a ses propres versions : le numéro est dans `natif/VERSION` et le tag git est `natif-vX.Y.Z`, distinct des tags `vX.Y.Z` du mode superposé (`beamng/`). Les règles sont les mêmes :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

## natif-v0.2.0
- Sons de Sonic par l'audio de BeamNG : pas selon le sol (béton, terre, herbe, sable, neige, eau, métal), atterrissage selon le sol, saut, boost (départ puis souffle en boucle tant qu'il boost), freinage, vent qui monte avec la vitesse, bang supersonique (boost relancé au-dessus de 200 km/h, choc contre une voiture au-dessus de 120 km/h). Les règles et les volumes sont ceux du mode superposé. Les sons suivent Sonic, même en boost.
- Plus riche que le superposé : toutes les variantes de Frontiers sont jouées au hasard (jusqu'à 12 bruits de pas différents sur la terre, au lieu d'un seul), et toutes les couches des sons à plusieurs couches (boost, saut, bang).
- Le convertisseur extrait ces sons de la banque de Frontiers du joueur en WAV (`mods/sonicnative_data/sounds`, 121 fichiers, 10 Mo) et calcule le moment où chaque pied touche le sol dans les animations de marche et de course. Les sons en boucle sont coupés à la boucle de Frontiers, avec un fondu de 20 ms à la jonction (sans lui, le souffle du boost faisait un petit clic à chaque tour).
- La détection du sol sous les pieds de Sonic, retirée en 0.1.0, revient (la même que le superposé).
- Sons coupés en pause et quand Sonic n'est pas à pied.
- `natif/build.ps1 -Install` met le mod à jour sur place au lieu de supprimer puis recopier son dossier : avec le jeu ouvert, BeamNG perdait la caméra de Sonic (image figée, commandes inversées).
- À savoir : le bang supersonique suit Sonic au lieu de rester sur place (les sons du mod sont accrochés à son corps).

## natif-v0.1.0
- Premier prototype du mode natif, sans DLL : Sonic est un vrai objet de la scène de BeamNG (un StaticShape), éclairé par le jeu et caché par ce qui passe devant lui. Il ne projette pas encore d'ombre.
- Un convertisseur lit Sonic dans le Sonic Frontiers du joueur (modèle, squelette, animations de repos, de marche, de course, de boost et de chute, textures) et écrit un modèle COLLADA animé, ses matériaux et ses textures dans le dossier utilisateur de BeamNG (`mods/sonicnative_data`). Rien de Frontiers n'est dans le dépôt ni dans le mod.
- Le mod `sonicnative` place Sonic aux pieds du personnage à pied de BeamNG, le tourne dans le sens de sa course, cache le personnage du jeu et choisit l'animation et sa vitesse selon son allure. La physique de Sonic (extension `sonicnative_body`) est reprise du mode superposé.
- `natif/build.ps1` vérifie le Lua, construit le convertisseur et installe le mod dans le dossier utilisateur de dev (ou dans le dossier normal).
- Testé dans BeamNG sur Gridmap V2 : Sonic apparaît texturé, à pied dès le chargement de la carte, avec sa caméra de poursuite ; course (jusqu'à environ 90 km/h), boost (près de 300 km/h, course sur un mur), saut (environ 3 m) avec l'animation de chute, montée et descente de voiture ; il est caché par une voiture placée devant lui.
- Si la carte a été lancée sans le mode marche de BeamNG (`gameplay_walk`), le mod le charge lui-même pour mettre Sonic à pied ; pendant les 3 s où le nouveau corps se met en place, la caméra de Sonic est remise si BeamNG la réinitialise (ensuite, la touche C reste au choix du joueur).
