struct Weapon;
struct Ped { Weapon* sel; Weapon* weapon; int pad[4]; void Copy(Ped* other); };
struct Group { Ped* leader; Ped* list[8]; void Promote(int idx); };

void Group::Promote(int idx)
{
    Weapon* leaderWeapon = leader->sel;
    Weapon* memberWeapon = list[idx]->sel;
    Weapon* leaderWeapon2 = leader->weapon;
    Weapon* memberWeapon2 = list[idx]->weapon;

    leader->Copy(list[idx]);
    leader->sel = leaderWeapon;
    leader->weapon = leaderWeapon2;
    list[idx]->sel = memberWeapon;
    list[idx]->weapon = memberWeapon2;
}
