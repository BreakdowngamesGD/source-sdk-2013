// Decompiled by XMT from /combine_wallhammer.mdl
// Source .mdl version 48, checksum -745851747

$modelname "combineskinpack1/combine_wallhammer.mdl"

$bodygroup "Soldier"
{
	studio "combine_wallhammer_Soldier_reference.smd"
}

$bodygroup "Stunbaton"
{
	blank
	studio "combine_wallhammer_Stunbaton_reference.smd"
}

$surfaceprop "flesh"

$contents "solid"

$illumposition 0.067 2.255 35.51

$eyeposition -0 0 73

$opaque

$cdmaterials "models/Combine_Soldier/"
$cdmaterials "models/hl2_wallhammer/"
$cdmaterials "models/weapons/w_stunbaton/"

$attachment "lefthand" "ValveBiped.Bip01_L_Hand" 0 0 -0 rotate 0.000002 0.000002 0.000001
$attachment "zipline" "ValveBiped.Bip01_Spine4" 2.999996 -3.999998 0 rotate -0 0 0
$attachment "eyes" "ValveBiped.Bip01_Head1" 4.5 -4.999999 -0 rotate 0 -90 -90
$attachment "beam_damage" "ValveBiped.Bip01_Spine2" 0 0 -0 rotate -0 -0 0
$attachment "anim_attachment_RH" "ValveBiped.Anim_Attachment_RH" 0 0 0 rotate -89.99999 -90 0
$attachment "anim_attachment_LH" "ValveBiped.Anim_Attachment_LH" 0 0 0 rotate -89.99999 -90 0
$attachment "shield_glow" "ValveBiped.Bip01_L_Forearm" 6.999998 0.5 6 rotate -0.000003 -0 -0.000001

$bbox -13 -13 0 13 13 72

// Hitboxes. $hbox takes the group, the bone, then the box in bone space.
$hboxset "default"
$hbox 1 "ValveBiped.Bip01_Head1" -2 -6.2 -5.5 9 4.8 5.5
$hbox 2 "ValveBiped.Bip01_Spine4" -2.5 -6.6 -7 9.5 5.4 7
$hbox 3 "ValveBiped.Bip01_Pelvis" -7.5 -1 -4.5 7.5 6 4.5
$hbox 3 "ValveBiped.Bip01_Spine" 0 -4 -6.25 6 5 6.25
$hbox 3 "ValveBiped.Bip01_Spine1" -3 -4.5 -6 3 4.5 6
$hbox 3 "ValveBiped.Bip01_Spine2" -1.5 -4.8 -6 4.5 4.2 6
$hbox 4 "ValveBiped.Bip01_L_UpperArm" -3.25 -2.55 -2.5 10.25 2.95 2.5
$hbox 4 "ValveBiped.Bip01_L_Forearm" -0.5 -1.75 -2.35 9.5 2.75 2.15
$hbox 4 "ValveBiped.Bip01_L_Hand" -1 -3 -2 5 1 3
$hbox 5 "ValveBiped.Bip01_R_UpperArm" -3.25 -2.55 -2.5 10.25 2.95 2.5
$hbox 5 "ValveBiped.Bip01_R_Forearm" -0.5 -1.75 -2.35 9.5 2.75 2.15
$hbox 5 "ValveBiped.Bip01_R_Hand" -1 -3 -3 5 1 2
$hbox 6 "ValveBiped.Bip01_L_Thigh" -1 -4.7 -3.25 17 4.3 4.25
$hbox 6 "ValveBiped.Bip01_L_Calf" -2 -3.5 -3.25 18 4.5 3.25
$hbox 6 "ValveBiped.Bip01_L_Foot" -2.5 -2.5 -2.4 5.5 4.9 2.6
$hbox 6 "ValveBiped.Bip01_L_Toe0" -0.8 -2.5 -2.4 4.2 0.5 2.7
$hbox 7 "ValveBiped.Bip01_R_Thigh" -1 -4.7 -4.25 17 4.3 3.25
$hbox 7 "ValveBiped.Bip01_R_Calf" -2 -3.5 -3.25 18 4.5 3.25
$hbox 7 "ValveBiped.Bip01_R_Foot" -2.5 -2.5 -2.6 5.5 4.9 2.4
$hbox 7 "ValveBiped.Bip01_R_Toe0" -0.8 -2.5 -2.4 4.2 0.5 2.7

// $definebone pins the skeleton so a recompile cannot re-derive a different one.
$definebone "ValveBiped.Bip01_Pelvis" "" -0.000006 -0.592226 42.803 0 0.000057 89.99998
$definebone "ValveBiped.Bip01_L_Thigh" "ValveBiped.Bip01_Pelvis" 4.317769 0.000004 0 2.96964 -89.70084 -89.99752
$definebone "ValveBiped.Bip01_L_Calf" "ValveBiped.Bip01_L_Thigh" 19.80857 0.000147 0.000002 0.000115 1.960032 -0.283786
$definebone "ValveBiped.Bip01_L_Foot" "ValveBiped.Bip01_L_Calf" 18.34036 -0.000007 -0 0.984685 -57.43462 0.504088
$definebone "ValveBiped.Bip01_L_Toe0" "ValveBiped.Bip01_L_Foot" 7.605774 -0.524769 0.413781 -0.199733 -31.82054 -0.060161
$definebone "ValveBiped.Bip01_R_Thigh" "ValveBiped.Bip01_Pelvis" -4.317769 0.000015 0.000005 2.970614 -90.29935 -90.00606
$definebone "ValveBiped.Bip01_R_Calf" "ValveBiped.Bip01_R_Thigh" 19.80856 -0.000037 -0.000004 -0.000172 1.959057 0.283156
$definebone "ValveBiped.Bip01_R_Foot" "ValveBiped.Bip01_R_Calf" 18.34035 -0.000131 0.000001 -1.114231 -57.42601 -0.122212
$definebone "ValveBiped.Bip01_R_Toe0" "ValveBiped.Bip01_R_Foot" 7.604727 -0.528607 -0.427957 0.372308 -31.85892 -3.502319
$definebone "ValveBiped.Bip01_Spine" "ValveBiped.Bip01_Pelvis" 0.000008 3.712555 -3.309426 4.944454 89.99969 89.99992
$definebone "ValveBiped.Bip01_Spine1" "ValveBiped.Bip01_Spine" 4.459698 0.000017 -0.000022 0.000057 -1.674813 -0
$definebone "ValveBiped.Bip01_Spine2" "ValveBiped.Bip01_Spine1" 3.905029 -0.000036 -0.000018 -0.002521 5.748715 0
$definebone "ValveBiped.Bip01_Spine4" "ValveBiped.Bip01_Spine2" 9.924873 -0.000056 -0.000473 0.002979 11.12174 0.000516
$definebone "ValveBiped.Bip01_Neck1" "ValveBiped.Bip01_Spine4" 3.670528 -0.000076 0.000018 -0.001146 21.37018 179.9997
$definebone "ValveBiped.Bip01_Head1" "ValveBiped.Bip01_Neck1" 3.986923 -0.109592 0.00006 0.000573 17.3471 -0.000229
$definebone "ValveBiped.Bip01_L_Clavicle" "ValveBiped.Bip01_Spine4" 2.256733 1.110637 2.1505 -63.94455 170.1681 89.6347
$definebone "ValveBiped.Bip01_L_UpperArm" "ValveBiped.Bip01_L_Clavicle" 6.592979 1.135284 0.057981 2.520098 -23.73048 -93.18471
$definebone "ValveBiped.Bip01_L_Forearm" "ValveBiped.Bip01_L_UpperArm" 12.97685 -0.000102 -0.000019 0.195947 -3.453961 -0.021029
$definebone "ValveBiped.Bip01_L_Hand" "ValveBiped.Bip01_L_Forearm" 12.74281 -0.000009 -0.000004 -6.115641 7.729201 90.15228
$definebone "ValveBiped.Bip01_L_Finger2" "ValveBiped.Bip01_L_Hand" 4.244747 -0.061378 0.888078 -1.872481 -33.41118 7.728514
$definebone "ValveBiped.Bip01_L_Finger21" "ValveBiped.Bip01_L_Finger2" 1.908455 -0.000008 0.000011 1.196338 -20.96803 -1.879074
$definebone "ValveBiped.Bip01_L_Finger22" "ValveBiped.Bip01_L_Finger21" 1.34198 0 -0.000004 0.17613 -12.39899 0.000056
$definebone "ValveBiped.Bip01_L_Finger1" "ValveBiped.Bip01_L_Hand" 4.123287 -0.209206 1.87515 -5.848694 -30.53636 21.97391
$definebone "ValveBiped.Bip01_L_Finger11" "ValveBiped.Bip01_L_Finger1" 1.908295 0.000011 -0.000013 1.454454 -20.75111 0.992362
$definebone "ValveBiped.Bip01_L_Finger12" "ValveBiped.Bip01_L_Finger11" 1.220459 -0.000002 0.000002 0.148453 -13.99874 0
$definebone "ValveBiped.Bip01_L_Finger0" "ValveBiped.Bip01_L_Hand" 0.752691 -0.452583 1.526144 -40.69204 -49.81943 -70.71456
$definebone "ValveBiped.Bip01_L_Finger01" "ValveBiped.Bip01_L_Finger0" 1.986374 0.000002 0.000006 -1.658536 12.99669 -8.520346
$definebone "ValveBiped.Bip01_L_Finger02" "ValveBiped.Bip01_L_Finger01" 1.339569 -0.000002 -0.000006 0.645445 20.79132 0.00074
$definebone "ValveBiped.Anim_Attachment_LH" "ValveBiped.Bip01_L_Hand" 2.970387 -1.889618 0.197792 -2.644947 90.43382 84.47707
$definebone "ValveBiped.Bip01_R_Clavicle" "ValveBiped.Bip01_Spine4" 2.256741 1.110645 -2.150424 64.04263 169.5726 -90.20834
$definebone "ValveBiped.Bip01_R_UpperArm" "ValveBiped.Bip01_R_Clavicle" 6.595102 1.124119 -0.027196 -2.743551 -23.83412 93.17595
$definebone "ValveBiped.Bip01_R_Forearm" "ValveBiped.Bip01_R_UpperArm" 12.97684 -0.00016 -0.000004 -0.186777 -3.454821 -0.01547
$definebone "ValveBiped.Bip01_R_Hand" "ValveBiped.Bip01_R_Forearm" 12.74283 0.000014 -0.000008 6.095245 7.463116 -89.59169
$definebone "ValveBiped.Bip01_R_Finger2" "ValveBiped.Bip01_R_Hand" 4.249071 -0.067905 -0.866543 1.411769 -33.42011 -7.075744
$definebone "ValveBiped.Bip01_R_Finger21" "ValveBiped.Bip01_R_Finger2" 1.908463 -0.000004 -0.000006 -1.49542 -20.94946 1.809573
$definebone "ValveBiped.Bip01_R_Finger22" "ValveBiped.Bip01_R_Finger21" 1.341988 -0.000006 -0.000006 -0.25531 -12.39658 -0.000172
$definebone "ValveBiped.Bip01_R_Finger1" "ValveBiped.Bip01_R_Hand" 4.132496 -0.222717 -1.853135 5.397265 -30.5599 -21.70124
$definebone "ValveBiped.Bip01_R_Finger11" "ValveBiped.Bip01_R_Finger1" 1.908287 0 0 -1.623878 -20.73935 -0.627045
$definebone "ValveBiped.Bip01_R_Finger12" "ValveBiped.Bip01_R_Finger11" 1.220444 0.000004 -0.000001 -0.240928 -13.99857 0.000171
$definebone "ValveBiped.Bip01_R_Finger0" "ValveBiped.Bip01_R_Hand" 0.760197 -0.463471 -1.51914 40.19815 -49.86085 88.85143
$definebone "ValveBiped.Bip01_R_Finger01" "ValveBiped.Bip01_R_Finger0" 1.98637 0.000004 0.000002 5.60628 11.85856 12.6512
$definebone "ValveBiped.Bip01_R_Finger02" "ValveBiped.Bip01_R_Finger01" 1.339588 0.000004 -0.000006 -0.551244 20.79229 -0.000511
$definebone "ValveBiped.Anim_Attachment_RH" "ValveBiped.Bip01_R_Hand" 2.969751 -1.891914 -0.185255 -2.587192 -89.67558 -95.23699
$definebone "ValveBiped.Cod" "ValveBiped.Bip01_Pelvis" -0.049633 2.5681 5.173502 -0.000057 0 -2.249948

$ikchain "rhand" "ValveBiped.Bip01_R_Hand" knee 0.707 0.707 0
$ikchain "lhand" "ValveBiped.Bip01_L_Hand" knee 0.707 0.707 0
$ikchain "rfoot" "ValveBiped.Bip01_R_Foot" knee 0.707 -0.707 0
$ikchain "lfoot" "ValveBiped.Bip01_L_Foot" knee 0.707 -0.707 0

$ikautoplaylock "rfoot" 0.5 0.1
$ikautoplaylock "lfoot" 0.5 0.1

$animation "a_proportions" "combine_wallhammer_anims/a_proportions.smd" {
	fps 30
}

$sequence "reference" {
	"combine_wallhammer_anims/reference.smd"
	fps 1
	fadein 0.2
	fadeout 0.2
}

$sequence "proportions" {
	"a_proportions"
	fps 30
	autoplay
	predelta
	fadein 0.2
	fadeout 0.2
}

$sequence "ragdoll" {
	"combine_wallhammer_anims/ragdoll.smd"
	fps 30
	activity "ACT_DIERAGDOLL" 1
	fadein 0.2
	fadeout 0.2
}

$sequence "flash" {
	"combine_wallhammer_anims/flash.smd"
	fps 25
	fadein 0.2
	fadeout 0.2
}

$sequence "melee_stunstick" {
	"combine_wallhammer_anims/melee_stunstick.smd"
	fps 30
	activity "ACT_MELEE_ATTACK2" 1
	{ event 3 18 }
	fadein 0.2
	fadeout 0.2
}

$sequence "melee_stunstick2" {
	"combine_wallhammer_anims/melee_stunstick2.smd"
	fps 30
	activity "ACT_MELEE_ATTACK3" 1
	{ event 3 27 }
	fadein 0.2
	fadeout 0.2
}

$includemodel "combine_soldier_anims.mdl"

$collisionjoints "combine_wallhammer_physics.smd"
{
	$mass 65
	$inertia 1
	$damping 0
	$rotdamping 0
	$concave
	$maxconvexpieces 18
}

// The .phy's own collision text, which holds the joint constraints and
// per-solid properties the block above only approximates.
$collisiontext
{
solid {
"index" "0"
"name" "ValveBiped.Bip01_Pelvis"
"mass" "3.910837"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "3.000000"
"inertia" "10.000000"
"volume" "473.418518"
}
solid {
"index" "1"
"name" "ValveBiped.Bip01_Spine1"
"parent" "ValveBiped.Bip01_Pelvis"
"mass" "10.417402"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "1261.057739"
}
solid {
"index" "2"
"name" "ValveBiped.Bip01_Spine2"
"parent" "ValveBiped.Bip01_Spine1"
"mass" "18.857086"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "2282.706543"
}
solid {
"index" "3"
"name" "ValveBiped.Bip01_R_Clavicle"
"parent" "ValveBiped.Bip01_Spine2"
"mass" "1.000000"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "94.312927"
}
solid {
"index" "4"
"name" "ValveBiped.Bip01_L_Clavicle"
"parent" "ValveBiped.Bip01_Spine2"
"mass" "1.000000"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "94.312805"
}
solid {
"index" "5"
"name" "ValveBiped.Bip01_L_UpperArm"
"parent" "ValveBiped.Bip01_L_Clavicle"
"mass" "1.072182"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "129.790802"
}
solid {
"index" "6"
"name" "ValveBiped.Bip01_L_Forearm"
"parent" "ValveBiped.Bip01_L_UpperArm"
"mass" "1.499679"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "181.540604"
}
solid {
"index" "7"
"name" "ValveBiped.Bip01_L_Hand"
"parent" "ValveBiped.Bip01_L_Forearm"
"mass" "1.000000"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "50.473270"
}
solid {
"index" "8"
"name" "ValveBiped.Bip01_R_UpperArm"
"parent" "ValveBiped.Bip01_R_Clavicle"
"mass" "1.067694"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "129.247574"
}
solid {
"index" "9"
"name" "ValveBiped.Bip01_R_Forearm"
"parent" "ValveBiped.Bip01_R_UpperArm"
"mass" "1.535328"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "185.856079"
}
solid {
"index" "10"
"name" "ValveBiped.Bip01_R_Hand"
"parent" "ValveBiped.Bip01_R_Forearm"
"mass" "1.000000"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "50.473160"
}
solid {
"index" "11"
"name" "ValveBiped.Bip01_R_Thigh"
"parent" "ValveBiped.Bip01_Pelvis"
"mass" "5.711276"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "691.367004"
}
solid {
"index" "12"
"name" "ValveBiped.Bip01_R_Calf"
"parent" "ValveBiped.Bip01_R_Thigh"
"mass" "3.431779"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "415.427094"
}
solid {
"index" "13"
"name" "ValveBiped.Bip01_R_Foot"
"parent" "ValveBiped.Bip01_R_Calf"
"mass" "1.152232"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "139.481155"
}
solid {
"index" "14"
"name" "ValveBiped.Bip01_L_Thigh"
"parent" "ValveBiped.Bip01_Pelvis"
"mass" "5.742965"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "695.203064"
}
solid {
"index" "15"
"name" "ValveBiped.Bip01_L_Calf"
"parent" "ValveBiped.Bip01_L_Thigh"
"mass" "3.390780"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "410.464081"
}
solid {
"index" "16"
"name" "ValveBiped.Bip01_L_Foot"
"parent" "ValveBiped.Bip01_L_Calf"
"mass" "1.144359"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "138.528046"
}
solid {
"index" "17"
"name" "ValveBiped.Bip01_Head1"
"parent" "ValveBiped.Bip01_Spine2"
"mass" "3.674291"
"surfaceprop" "flesh"
"damping" "0.050000"
"rotdamping" "10.000000"
"inertia" "10.000000"
"volume" "444.783966"
}
ragdollconstraint {
"parent" "0"
"child" "1"
"xmin" "-20.000000"
"xmax" "20.000000"
"xfriction" "0.000000"
"ymin" "-10.000000"
"ymax" "10.000000"
"yfriction" "0.000000"
"zmin" "-20.000000"
"zmax" "10.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "1"
"child" "2"
"xmin" "-20.000000"
"xmax" "20.000000"
"xfriction" "0.000000"
"ymin" "-15.000000"
"ymax" "15.000000"
"yfriction" "0.000000"
"zmin" "-30.000000"
"zmax" "45.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "2"
"child" "3"
"xmin" "-10.000000"
"xmax" "10.000000"
"xfriction" "0.000000"
"ymin" "-20.000000"
"ymax" "20.000000"
"yfriction" "0.000000"
"zmin" "-5.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "2"
"child" "4"
"xmin" "-10.000000"
"xmax" "10.000000"
"xfriction" "0.000000"
"ymin" "-20.000000"
"ymax" "20.000000"
"yfriction" "0.000000"
"zmin" "-5.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "4"
"child" "5"
"xmin" "-45.000000"
"xmax" "45.000000"
"xfriction" "0.000000"
"ymin" "-45.000000"
"ymax" "50.000000"
"yfriction" "0.000000"
"zmin" "-90.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "5"
"child" "6"
"xmin" "0.000000"
"xmax" "0.000000"
"xfriction" "0.000000"
"ymin" "0.000000"
"ymax" "0.000000"
"yfriction" "0.000000"
"zmin" "-130.000000"
"zmax" "0.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "6"
"child" "7"
"xmin" "-45.000000"
"xmax" "45.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-30.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "3"
"child" "8"
"xmin" "-45.000000"
"xmax" "45.000000"
"xfriction" "0.000000"
"ymin" "-50.000000"
"ymax" "45.000000"
"yfriction" "0.000000"
"zmin" "-90.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "8"
"child" "9"
"xmin" "0.000000"
"xmax" "0.000000"
"xfriction" "0.000000"
"ymin" "0.000000"
"ymax" "0.000000"
"yfriction" "0.000000"
"zmin" "-130.000000"
"zmax" "0.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "9"
"child" "10"
"xmin" "-45.000000"
"xmax" "45.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-30.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "0"
"child" "11"
"xmin" "-15.000000"
"xmax" "15.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-60.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "11"
"child" "12"
"xmin" "0.000000"
"xmax" "0.000000"
"xfriction" "0.000000"
"ymin" "0.000000"
"ymax" "0.000000"
"yfriction" "0.000000"
"zmin" "0.000000"
"zmax" "110.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "12"
"child" "13"
"xmin" "-30.000000"
"xmax" "30.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-30.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "0"
"child" "14"
"xmin" "-15.000000"
"xmax" "15.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-60.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "14"
"child" "15"
"xmin" "0.000000"
"xmax" "0.000000"
"xfriction" "0.000000"
"ymin" "0.000000"
"ymax" "0.000000"
"yfriction" "0.000000"
"zmin" "0.000000"
"zmax" "110.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "15"
"child" "16"
"xmin" "-30.000000"
"xmax" "30.000000"
"xfriction" "0.000000"
"ymin" "-30.000000"
"ymax" "30.000000"
"yfriction" "0.000000"
"zmin" "-30.000000"
"zmax" "30.000000"
"zfriction" "0.000000"
}
ragdollconstraint {
"parent" "2"
"child" "17"
"xmin" "-45.000000"
"xmax" "45.000000"
"xfriction" "0.000000"
"ymin" "-10.000000"
"ymax" "10.000000"
"yfriction" "0.000000"
"zmin" "-15.000000"
"zmax" "15.000000"
"zfriction" "0.000000"
}
editparams {
"rootname" "valvebiped.bip01_pelvis"
"totalmass" "65.000000"
}
}

$keyvalues
{
mdlkeyvalue
{
physgun_interactions {
"onpickup" "boogie" "onlaunch" "spin_zaxis" "onfirstimpact" "bloodsplat"  }
}
}
