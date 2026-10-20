@echo off
rem Lance BeamNG.drive (Steam) sur le dossier de test de Sonic Beam, sans avertissement de Steam.
rem A faire une seule fois : dans Steam, clic droit sur BeamNG.drive > Proprietes > General >
rem Options de lancement, coller SANS guillemets (le lanceur de BeamNG plante avec) :
rem   -userpath <your Sonic Beam folder>\beamng\devuser
rem (pour revenir a ton BeamNG normal, il suffit de vider ce champ).
start "" "steam://rungameid/284160"
